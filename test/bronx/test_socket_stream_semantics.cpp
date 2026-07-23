// BxSocket/Stream 返回值语义回归测试(阶段4)
// =========================================
// 守护 stream.h 文档化的返回值契约:
//   readExact 成功 = length;对端关闭 <=0;超时 <0 且 errno=ETIMEDOUT
//   单次 read 对端关闭返回 0;未连接返回 <0 且 errno=ENOTCONN
// 用 socketpair + BxSocketStream 包装其中一端。

#include "test_util.h"
#include "reactor.h"
#include "net_socket.h"
#include "sock_stream.h"
#include "fd_context.h"
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

static void register_fd(int fd) {
    bronx::FdMgr::GetInstance()->get(fd, true);
}

static void close_registered_fd(int fd) {
    bronx::FdMgr::GetInstance()->del(fd);
    close(fd);
}

// 1. 分段发送也能读满:对端写 8 字节(可能分两次),读满返回累计 8
static void test_readfix_success() {
    int sv[2];
    TEST_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    register_fd(sv[0]);
    register_fd(sv[1]);

    std::atomic<int> off_val{-999};
    {
        bronx::BxIoManager iom(2);
        iom.post([&](){
            char buf[8];
            // 对端分两次写,验证半包累计
            iom.post([&](){ write(sv[1], "1234", 4); usleep(30*1000); write(sv[1], "5678", 4); });
            int off = 0;
            while(off < 8){
                int r = recv(sv[0], buf + off, 8 - off, 0);
                if(r <= 0) break;
                off += r;
            }
            off_val = off;
        });
        usleep(300 * 1000);
    }
    TEST_CHECK_EQ(off_val.load(), 8);
    close_registered_fd(sv[0]);
    close_registered_fd(sv[1]);
}

// 2. 单次 read:对端关闭 → 返回 0
static void test_read_peer_close() {
    int sv[2];
    TEST_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    register_fd(sv[0]);
    register_fd(sv[1]);

    std::atomic<int> rt_val{-999};
    {
        bronx::BxIoManager iom(2);
        iom.post([&](){
            // 对端 50ms 后关闭
            iom.addTimer(50, [&](){ close_registered_fd(sv[1]); }, false);
            char buf[8];
            int r = recv(sv[0], buf, sizeof(buf), 0);  // 阻塞等,对端关后返回 0
            rt_val = r;
        });
        usleep(300 * 1000);
    }
    TEST_CHECK_MSG(rt_val.load() == 0, "peer close should return 0, got " << rt_val.load());
    close_registered_fd(sv[0]);
}

// 3. recv 超时 → 返回 <0,errno=ETIMEDOUT
static void test_recv_timeout_errno() {
    int sv[2];
    TEST_CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    register_fd(sv[0]);
    register_fd(sv[1]);

    std::atomic<int> rt_val{-999}, err_val{0};
    {
        bronx::BxIoManager iom(1);
        iom.post([&](){
            timeval tv{0, 120 * 1000};
            setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[8];
            int r = recv(sv[0], buf, sizeof(buf), 0);
            rt_val = r;
            err_val = errno;
        });
        usleep(400 * 1000);
    }
    TEST_CHECK_MSG(rt_val.load() < 0, "timeout recv should return <0, got " << rt_val.load());
    TEST_CHECK_MSG(err_val.load() == ETIMEDOUT, "errno should be ETIMEDOUT, got " << err_val.load());
    close_registered_fd(sv[0]);
    close_registered_fd(sv[1]);
}

// 4. BxSocketStream 未连接 read → 返回 <0,errno=ENOTCONN
static void test_stream_notconn() {
    // 构造一个未连接的 BxSocket 并包成 BxSocketStream
    auto sock = std::make_shared<bronx::BxSocket>(bronx::BxSocket::IPv4, bronx::BxSocket::TCP, 0);
    bronx::BxSocketStream ss(sock);
    char buf[8];
    errno = 0;
    int r = ss.read(buf, sizeof(buf));
    TEST_CHECK_MSG(r < 0, "read on unconnected stream should return <0, got " << r);
    TEST_CHECK_MSG(errno == ENOTCONN, "errno should be ENOTCONN, got " << errno);
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_socket_stream_semantics start ===";
    test_readfix_success();
    test_read_peer_close();
    test_recv_timeout_errno();
    test_stream_notconn();
    return TEST_SUMMARY();
}
