// 测试用举报注入器。往 daemon 的 submit 口发一条 RISK, 模拟 WAF/限流上报。
// 用法: ./fakerisk <ip> [banMs] [src] [submit.sock]
//   ./fakerisk 1.2.3.4 60000 rate /tmp/bronx_ip_submit.sock
#include "proto.h"
#include "wire.h"
#include "rule.h"
#include "reactor.h"
#include "endpoint.h"
#include "log.h"
#include <atomic>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace bronx::ipban;
static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

static void send_one(const std::string& ipStr, uint64_t banMs,
                     const std::string& src, const std::string& sockPath) {
    auto sock = bronx::BxSocket::MakeUnixTcpSocket();
    auto addr = bronx::BxUnixAddress::Create(sockPath);
    if(!sock || !addr || !sock->connect(addr, 2000)) {
        BRONX_LOG_ERROR(g_logger) << "connect submit sock failed: " << sockPath;
        return;
    }
    Risk r;
    r.id = "fake-" + ipStr;
    if(!parseCidr(ipStr, r.ip)) {
        BRONX_LOG_ERROR(g_logger) << "bad ip: " << ipStr;
        return;
    }
    r.src = srcFromName(src);
    r.banMs = banMs;
    r.reason = "fakerisk manual";
    if(sendMsg(sock, Kind::RISK, riskToJson(r))) {
        BRONX_LOG_INFO(g_logger) << "sent risk ip=" << ipStr << " ban=" << banMs
                                 << "ms src=" << src;
    } else {
        BRONX_LOG_ERROR(g_logger) << "send risk failed";
    }
    sock->close();
}

int main(int argc, char** argv) {
    if(argc < 2) {
        BRONX_LOG_ERROR(g_logger) << "usage: fakerisk <ip> [banMs] [src] [submit.sock]";
        return 1;
    }
    std::string ip = argv[1];
    uint64_t banMs = argc > 2 ? strtoull(argv[2], nullptr, 10) : 60000;
    std::string src = argc > 3 ? argv[3] : "rate";
    std::string sockPath = argc > 4 ? argv[4] : "/tmp/bronx_ip_submit.sock";

    // 一次性发送。send/connect 要在 worker 协程里才走 hook, 所以 post 进去发。
    // 别在协程里 stop 自己的 iom(会和 main 的析构撞), 用 done 标志让主线程等完再退。
    std::atomic<bool> done{false};
    bronx::BxIoManager iom(1);
    iom.post([&]() {
        send_one(ip, banMs, src, sockPath);
        done.store(true);
    });
    for(int i = 0; i < 500 && !done.load(); ++i) usleep(10 * 1000);   // 最多等 5s
    iom.stop();
    return done.load() ? 0 : 1;
}
