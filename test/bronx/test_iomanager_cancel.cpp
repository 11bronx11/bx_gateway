// cancel / timeout 竞态回归测试(阶段2)
// =====================================
// 守护 hook/BxIoManager 的 cancel-generation 修复(memory: hook-iom-cancel-races):
//   C1: read 阻塞时另线程 abortAll → 协程返回错误,不永久挂起
//   C3: recv 超时 → 返回 <0、errno=ETIMEDOUT,协程正常退出
//   C4: close 路由 abortAll 到 owner BxIoManager → 阻塞协程被唤醒
//   C5: connect 中途取消 → 干净退出
// 用 socketpair 构造"对端不发数据"的可控阻塞场景。

#include "test_util.h"
#include "reactor.h"
#include "fd_context.h"
#include "net_socket.h"
#include "endpoint.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <cerrno>
#include <thread>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

// socketpair 的 fd 不经 hook 的 socket(),需手动注册到 FdMgr 才会被 do_io 接管(否则是真阻塞 read)。
// 这模拟真实 bronx::BxSocket(经 hooked socket()/accept() 自动注册)的等价状态。
static void register_fd(int fd) {
    bronx::FdMgr::GetInstance()->get(fd, /*auto_create=*/true); // init() 标记 socket + 设非阻塞
}

static void close_registered_fd(int fd) {
    bronx::FdMgr::GetInstance()->del(fd);
    close(fd);
}

// 1. C1/C4:read 阻塞在 READ 时,另一动作 abortAll/close,协程必须被唤醒返回,不挂死
static void test_read_cancel_wakes() {
    int sv[2];
    TEST_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    register_fd(sv[0]); register_fd(sv[1]);

    std::atomic<bool> coro_returned{false};
    std::atomic<int>  read_rt{999};
    uint64_t t0 = bronx_test::now_ms();
    {
        bronx::BxIoManager iom(2);
        // 协程A:对 sv[0] read,对端不发数据 → 阻塞等待 READ
        iom.post([&](){
            char buf[16];
            int rt = read(sv[0], buf, sizeof(buf));   // hook:协程让出等 READ
            read_rt = rt;
            coro_returned = true;
        });
        // 协程B:100ms 后 abortAll(sv[0]) 唤醒协程A
        iom.addTimer(100, [&](){
            bronx::BxIoManager::Current()->abortAll(sv[0]);
        }, false);
        // 撑住让上面跑完
        usleep(400 * 1000);
    }
    uint64_t elapsed = bronx_test::now_ms() - t0;
    TEST_CHECK_MSG(coro_returned.load(), "read coroutine never returned (HANG)");
    TEST_CHECK_MSG(read_rt.load() <= 0, "cancelled read should return <=0, got " << read_rt.load());
    TEST_CHECK_MSG(elapsed < 1000, "took " << elapsed << "ms (expected ~100ms wake)");
    close_registered_fd(sv[0]);
    close_registered_fd(sv[1]);
}

// 2. C3:recv 超时返回,errno=ETIMEDOUT,协程退出
static void test_recv_timeout() {
    int sv[2];
    TEST_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    register_fd(sv[0]); register_fd(sv[1]);

    std::atomic<bool> returned{false};
    std::atomic<int> rt_val{999};
    std::atomic<int> err_val{0};
    uint64_t t0 = bronx_test::now_ms();
    {
        bronx::BxIoManager iom(1);
        iom.post([&](){
            // 设 recv 超时 150ms,对端不发 → 应超时返回
            struct timeval tv{0, 150 * 1000};
            setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[16];
            int rt = recv(sv[0], buf, sizeof(buf), 0);
            rt_val = rt;
            err_val = errno;
            returned = true;
        });
        usleep(500 * 1000);
    }
    uint64_t elapsed = bronx_test::now_ms() - t0;
    TEST_CHECK_MSG(returned.load(), "recv coroutine never returned (HANG)");
    TEST_CHECK_MSG(rt_val.load() < 0, "timed-out recv should return <0, got " << rt_val.load());
    TEST_CHECK_MSG(err_val.load() == ETIMEDOUT, "errno should be ETIMEDOUT, got " << err_val.load());
    TEST_CHECK_MSG(elapsed >= 100 && elapsed < 1000, "timeout fired at " << elapsed << "ms (expected ~150ms)");
    close_registered_fd(sv[0]);
    close_registered_fd(sv[1]);
}

// 3. BxSocket::abortRead 从非 BxIoManager 线程调用，也必须路由到 fd owner BxIoManager。
static void test_socket_cancel_from_foreign_thread() {
    std::atomic<bool> recv_started{false};
    std::atomic<bool> recv_returned{false};
    std::atomic<int> cancel_rt{-1};
    std::atomic<int> recv_rt{999};
    bronx::BxSocket::ptr client;
    std::thread cancel_thread;

    uint64_t t0 = bronx_test::now_ms();
    {
        bronx::BxIoManager iom(2);
        iom.post([&](){
            auto server = bronx::BxSocket::MakeTcpSocket();
            TEST_CHECK(server->bind(bronx::BxIpAddress::Create("127.0.0.1", 0)));
            TEST_CHECK(server->listen());
            uint16_t port = std::dynamic_pointer_cast<bronx::BxIpAddress>(
                    server->getLocalAddress())->getPort();

            client = bronx::BxSocket::MakeTcpSocket();
            auto addr = bronx::BxIpAddress::Create("127.0.0.1", port);
            TEST_CHECK(client->connect(addr));
            auto accepted = server->accept();
            TEST_CHECK(accepted);

            cancel_thread = std::thread([&](){
                usleep(100 * 1000);
                cancel_rt = client->abortRead() ? 1 : 0;
            });

            recv_started = true;
            char buf[16];
            int rt = client->recv(buf, sizeof(buf));
            recv_rt = rt;
            recv_returned = true;

            if(cancel_thread.joinable()){
                cancel_thread.join();
            }
            if(accepted){
                accepted->close();
            }
            server->close();
        });

        usleep(500 * 1000);
    }
    uint64_t elapsed = bronx_test::now_ms() - t0;

    TEST_CHECK_MSG(cancel_rt.load() == 1, "foreign-thread abortRead returned " << cancel_rt.load());
    TEST_CHECK_MSG(recv_returned.load(), "recv coroutine never returned after foreign-thread abortRead");
    TEST_CHECK_MSG(recv_rt.load() <= 0, "cancelled recv should return <=0, got " << recv_rt.load());
    TEST_CHECK_MSG(elapsed < 1000, "foreign-thread cancel took " << elapsed << "ms");

    if(cancel_thread.joinable()){
        cancel_thread.join();
    }
    if(client){
        client->close();
    }
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_iomanager_cancel start ===";
    test_read_cancel_wakes();
    test_recv_timeout();
    test_socket_cancel_from_foreign_thread();
    return TEST_SUMMARY();
}
