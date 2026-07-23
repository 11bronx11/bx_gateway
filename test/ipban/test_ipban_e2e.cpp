// 阶段4 端到端: daemon + sync + reporter 全在一个进程里, 走真 UDS 通信。
// 验证 举报 -> daemon 存 -> 网关拉到 -> eval DENY -> TTL 到期 -> eval 放行。
// daemon 跑自己的 IoManager, 网关侧(sync/reporter/driver)跑另一个, 模拟两进程。
#include "test_util.h"
#include "daemon.h"
#include "pull.h"
#include "report.h"
#include "guard.h"
#include "reactor.h"
#include "util.h"
#include <atomic>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace bronx::ipban;

static Ip mkIp(const char* s) { Ip ip; parseCidr(s, ip); return ip; }

// 轮询等 eval 变成期望动作, 最多等 timeoutMs。命中返回 true。
static bool waitAction(Guard::ptr g, const Ip& ip, Act want, uint64_t timeoutMs) {
    uint64_t deadline = bronx::GetCurrentMs() + timeoutMs;
    while(bronx::GetCurrentMs() < deadline) {
        if(g->eval(ip).action == want) return true;
        usleep(50 * 1000);   // hook 让出, 让 sync/reporter 协程跑
    }
    return g->eval(ip).action == want;
}

static void testRegularFileIsKept() {
    std::string path = "/tmp/bronx_ipban_regular_" + std::to_string(::getpid());
    {
        std::ofstream out(path, std::ios::trunc);
        out << "keep";
    }
    bronx::BxIoManager iom(1, "path");
    DaemonOpts opts;
    opts.submitPath = path + ".sock";
    opts.subscribePath = path;
    Daemon daemon(&iom, opts);
    TEST_CHECK_MSG(!daemon.start(), "regular file must not be unlinked for socket bind");
    std::ifstream in(path);
    std::string text;
    in >> text;
    TEST_CHECK_EQ(text, std::string("keep"));
    TEST_CHECK(::access(opts.submitPath.c_str(), F_OK) != 0);
    ::unlink(path.c_str());
}

int main() {
    testRegularFileIsKept();
    std::string base = "/tmp/bronx_ipban_e2e_" + std::to_string(::getpid());
    std::string submitPath = base + "_submit.sock";
    std::string subPath    = base + "_sub.sock";

    DaemonOpts dopts;
    dopts.submitPath = submitPath;
    dopts.subscribePath = subPath;
    dopts.expiryTickMs = 100;    // 快点扫过期(推送已是事件驱动, 无需轮询间隔)

    SyncOpts sopts;
    sopts.subscribePath = subPath;
    sopts.instanceId = "gw-e2e";

    ReportOpts ropts;
    ropts.submitPath = submitPath;

    bronx::BxIoManager iomD(1, "daemon");
    bronx::BxIoManager iomG(1, "gw");

    auto guard = std::make_shared<Guard>();
    guard->setEnabled(true);
    auto daemon = std::make_shared<Daemon>(&iomD, dopts);
    auto sync = std::make_shared<SyncClient>(guard, sopts);
    auto reporter = std::make_shared<Reporter>(ropts);

    std::atomic<bool> done{false};

    iomD.post([&]() {
        if(!daemon->start()) { TEST_FAIL("daemon start failed"); done.store(true); }
    });

    iomG.post([&]() {
        sync->start(&iomG);
        reporter->start(&iomG);

        Ip bad = mkIp("1.2.3.4");
        // 起手应放行(没规则)
        TEST_CHECK_MSG(guard->eval(bad).action == Act::ALLOW, "起手无规则应放行");

        // 等订阅连上并收到第一版空快照(remoteVersion 变过或稳定), 给 1.5s 建链
        usleep(1500 * 1000);

        // 举报: 封 1.2.3.4 2500ms(留足网关拉到并观测 DENY 的窗口)
        Risk r;
        r.id = "e2e-1"; r.ip = bad; r.src = Src::RATE; r.banMs = 2500;
        r.atMs = bronx::GetCurrentMs(); r.reason = "e2e abuse";
        TEST_CHECK_MSG(reporter->tryReport(r), "入队应成功");

        // 等网关拉到规则 -> DENY(举报->存->推->应用, 3s 够)
        TEST_CHECK_MSG(waitAction(guard, bad, Act::DENY, 3000),
                       "举报后应被封 DENY");
        TEST_CHECK(guard->remoteVersion() > 0);

        // 等 TTL 到期 -> 放行(2.5s 封期 + daemon 扫+推, 给 5s)
        TEST_CHECK_MSG(waitAction(guard, bad, Act::ALLOW, 5000),
                       "TTL 到期后应放行");

        // 别的 IP 全程放行
        TEST_CHECK(guard->eval(mkIp("9.9.9.9")).action == Act::ALLOW);

        sync->stop();
        reporter->stop();
        done.store(true);
    });

    // 主线程等完成, 最多 12s 兜底
    uint64_t deadline = bronx::GetCurrentMs() + 12000;
    while(!done.load() && bronx::GetCurrentMs() < deadline) {
        usleep(50 * 1000);
    }
    if(!done.load()) TEST_FAIL("e2e timed out");

    daemon->stop();   // 关监听口, 让 accept 协程退出, iomD.stop 才收得干净
    iomD.stop();
    iomG.stop();
    ::unlink(submitPath.c_str());
    ::unlink(subPath.c_str());
    return TEST_SUMMARY();
}
