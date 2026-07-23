// daemon 重启恢复: 头号 bug 的回归守卫。无持久化 daemon 重启后版本回退,
// 老守卫会把低版本快照当陈旧挡死 -> 网关抱着旧规则永不收敛。epoch 变了应无条件重置。
// 场景: daemon1 封 1.2.3.4 -> 网关 DENY -> daemon1 挂, daemon2 起(新 epoch, 空规则)
//       -> 网关重连收到低版本空快照 -> 应放行 1.2.3.4, 且能收 daemon2 新下发的封禁。
#include "test_util.h"
#include "daemon.h"
#include "pull.h"
#include "report.h"
#include "guard.h"
#include "reactor.h"
#include "util.h"
#include <atomic>
#include <string>
#include <unistd.h>

using namespace bronx::ipban;

static Ip mkIp(const char* s) { Ip ip; parseCidr(s, ip); return ip; }

static bool waitAction(Guard::ptr g, const Ip& ip, Act want, uint64_t timeoutMs) {
    uint64_t deadline = bronx::GetCurrentMs() + timeoutMs;
    while(bronx::GetCurrentMs() < deadline) {
        if(g->eval(ip).action == want) return true;
        usleep(50 * 1000);
    }
    return g->eval(ip).action == want;
}

int main() {
    std::string base = "/tmp/bronx_ipban_restart_" + std::to_string(::getpid());
    std::string submitPath = base + "_submit.sock";
    std::string subPath    = base + "_sub.sock";

    DaemonOpts dopts;
    dopts.submitPath = submitPath;
    dopts.subscribePath = subPath;
    dopts.expiryTickMs = 100;

    SyncOpts sopts; sopts.subscribePath = subPath; sopts.instanceId = "gw-restart";
    ReportOpts ropts; ropts.submitPath = submitPath;

    auto guard = std::make_shared<Guard>();
    guard->setEnabled(true);
    bronx::BxIoManager iomG(1, "gw");
    auto sync = std::make_shared<SyncClient>(guard, sopts);
    auto reporter = std::make_shared<Reporter>(ropts);

    Ip bad = mkIp("1.2.3.4");
    Ip fresh = mkIp("5.6.7.8");

    // --- daemon1: 封 1.2.3.4 永久 ---
    auto iomD1 = std::make_shared<bronx::BxIoManager>(1, "d1");
    auto d1 = std::make_shared<Daemon>(iomD1.get(), dopts);
    iomD1->post([&]() { if(!d1->start()) TEST_FAIL("d1 start failed"); });

    iomG.post([&]() {
        sync->start(&iomG);
        reporter->start(&iomG);
        usleep(800 * 1000);   // 等订阅建链
        Risk r; r.id="rst-1"; r.ip=bad; r.src=Src::RATE; r.banMs=0;   // 永久封
        r.atMs=bronx::GetCurrentMs(); r.reason="restart test";
        reporter->tryReport(r);
    });

    TEST_CHECK_MSG(waitAction(guard, bad, Act::DENY, 4000), "daemon1 封后应 DENY");
    uint64_t verBefore = guard->remoteVersion();
    uint64_t epBefore  = guard->remoteEpoch();
    TEST_CHECK(verBefore > 0);

    // --- daemon1 挂掉 ---
    d1->stop();
    iomD1->stop();
    d1.reset();
    iomD1.reset();
    usleep(300 * 1000);

    // --- daemon2 起在同样的 socket 上(新 PolicyEngine = 新 epoch, 规则空, 版本从 0) ---
    auto iomD2 = std::make_shared<bronx::BxIoManager>(1, "d2");
    auto d2 = std::make_shared<Daemon>(iomD2.get(), dopts);
    iomD2->post([&]() { if(!d2->start()) TEST_FAIL("d2 start failed"); });

    // 网关重连收到新 epoch 的低版本空快照 -> 应放行(老守卫会挡死一直 DENY)
    TEST_CHECK_MSG(waitAction(guard, bad, Act::ALLOW, 8000),
                   "daemon 重启后旧封禁应清掉(epoch 变则重置守卫)");
    TEST_CHECK_MSG(guard->remoteEpoch() != epBefore, "epoch 应换成新实例的");

    // daemon2 下发新封禁, 网关应能收到(证明重连后链路通)
    iomG.post([&]() {
        Risk r; r.id="rst-2"; r.ip=fresh; r.src=Src::RATE; r.banMs=0;
        r.atMs=bronx::GetCurrentMs(); r.reason="post restart";
        reporter->tryReport(r);
    });
    TEST_CHECK_MSG(waitAction(guard, fresh, Act::DENY, 5000), "重启后新封禁应生效");

    sync->stop();
    reporter->stop();
    usleep(200 * 1000);
    d2->stop();
    iomD2->stop();
    iomG.stop();
    ::unlink(submitPath.c_str());
    ::unlink(subPath.c_str());
    return TEST_SUMMARY();
}
