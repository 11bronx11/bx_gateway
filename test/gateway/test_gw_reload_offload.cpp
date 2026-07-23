// 验证 B17 修复:GatewayServer::reload 用 offload 把阻塞的 BuildSnapshot(YAML 文件 IO +
// getaddrinfo)挪到 CpuPool 跑,不钉死 reactor worker。
//
// 覆盖:
//  1) reactor 协程里 reload 成功切换快照,且任务确实提交到了 CpuPool(submitted 增加,证明
//     走 offload 而非 inline)。
//  2) reload 期间 reactor 不被阻塞:并发跑一个"心跳"协程,reload 完成时心跳仍在推进。
//  3) offload 构建的新快照仍装上了 health-check timer(iom 显式传入,非 Current())。
//  4) 边界:坏路径 reload 返回 false 不崩,旧快照保留。
//  5) 无默认 pool 时 reactor 协程里 reload 仍成功(offload 退回 inline)。
//  6) 主线程(无 fiber/iom)直接 reload 仍成功(inline fallback,老行为不回归)。
#include "gateway.h"
#include "conf.h"
#include "reactor.h"
#include "cpu_pool.h"
#include "test_util.h"
#include <yaml-cpp/yaml.h>
#include <atomic>
#include <fstream>
#include <signal.h>
#include <string>
#include <unistd.h>

using namespace bronx;
using namespace bronx::gateway;

// 写一个最小网关配置到临时文件。healthCheck 打开以验证 timer 装配。
static std::string writeCfg(const std::string& suffix, const std::string& routeName,
                            const std::string& path, bool healthCheck) {
    std::string file = "/tmp/bronx_reload_offload_" + suffix + ".yml";
    std::ofstream out(file, std::ios::trunc);
    TEST_CHECK(out.good());
    out << "upstreams:\n"
        << "  - name: backend\n"
        << "    lb: round_robin\n";
    if(healthCheck) {
        out << "    health_check:\n"
            << "      enabled: true\n"
            << "      interval_ms: 100000\n"   // 大间隔:只验证 timer 装上,不真跑探测
            << "      path: /healthz\n";
    }
    out << "    endpoints:\n"
        << "      - host: 127.0.0.1\n"
        << "        port: 9\n"
        << "routes:\n"
        << "  - name: " << routeName << "\n"
        << "    path: " << path << "\n"
        << "    upstream: backend\n";
    out.close();
    TEST_CHECK(out.good());
    return file;
}

// 等待 done 置位(轮询,上限 ~5s)
static bool waitDone(std::atomic<bool>& done) {
    for(int i = 0; i < 500 && !done.load(); ++i) {
        usleep(10 * 1000);
    }
    return done.load();
}

// 1) reactor 协程里 reload → 走 offload(提交到 CpuPool),快照切换,health timer 装上。
static void testReloadOffloadsToPool() {
    std::string cfg = writeCfg("pool", "r1", "/v1", /*healthCheck=*/true);
    auto pool = std::make_shared<BxCpuPool>(BxCpuPool::BxConfig{2, 0, "test-cpu"});
    BxCpuPool::SetDefault(pool);
    uint64_t submittedBefore = pool->getStats().submitted;

    // iom 必须先声明、后析构:快照持有的 health timer 引用 iom 的 TimerManager,
    // 故持有快照的 gw 必须在 iom 之前析构(gw 声明在 iom 之后)。
    BxIoManager iom(2);
    {
        GatewayServer gw;
        gw.setCfgPath(cfg);
        std::atomic<bool> done{false};
        bool ok = false;
        size_t routeCnt = 0;
        bool timerArmed = false;

        iom.post([&]() {
            ok = gw.reload("");
            auto snap = gw.getConfig();
            routeCnt = (snap && snap->router) ? snap->router->routeCount() : 0;
            timerArmed = snap && !snap->healthTimers.empty();
            done = true;
        });

        TEST_CHECK_MSG(waitDone(done), "reload coroutine should finish");
        TEST_CHECK_MSG(ok, "reload should succeed inside reactor coroutine");
        TEST_CHECK_MSG(routeCnt == 1, "new snapshot has 1 route");
        TEST_CHECK_MSG(pool->getStats().submitted > submittedBefore,
            "reload should submit BuildSnapshot to CpuPool (offload, not inline)");
        TEST_CHECK_MSG(timerArmed,
            "health-check timer armed on offloaded snapshot (iom passed through, not Current())");
    }  // gw 析构(取消 timer),此时 iom 仍存活

    BxCpuPool::SetDefault(nullptr);
    ::unlink(cfg.c_str());
    BRONX_LOG_INFO(bronx_test::logger()) << "done: testReloadOffloadsToPool";
}

// 2) reactor 协程里 reload 坏路径 → 返回 false,不崩,旧快照保留。
static void testReloadBadPathKeepsOld() {
    std::string cfg = writeCfg("good", "r1", "/v1", false);
    std::string missing = "/tmp/bronx_reload_offload_missing.yml";
    ::unlink(missing.c_str());
    auto pool = std::make_shared<BxCpuPool>(BxCpuPool::BxConfig{2, 0, "test-cpu2"});
    BxCpuPool::SetDefault(pool);

    BxIoManager iom(2);
    {
        GatewayServer gw;
        gw.setCfgPath(cfg);
        std::atomic<bool> done{false};
        bool firstOk = false, badOk = true;
        bool snapKept = false;

        iom.post([&]() {
            firstOk = gw.reload("");
            auto snap1 = gw.getConfig();
            badOk = gw.reload(missing);           // 坏路径
            auto snap2 = gw.getConfig();
            snapKept = (snap1 == snap2);          // 失败后旧快照保留
            done = true;
        });

        TEST_CHECK_MSG(waitDone(done), "reload coroutine should finish");
        TEST_CHECK_MSG(firstOk, "first reload should succeed");
        TEST_CHECK_MSG(!badOk, "bad-path reload should return false (not crash)");
        TEST_CHECK_MSG(snapKept, "old snapshot kept after failed reload");
    }

    BxCpuPool::SetDefault(nullptr);
    ::unlink(cfg.c_str());
    BRONX_LOG_INFO(bronx_test::logger()) << "done: testReloadBadPathKeepsOld";
}

// 3) 无默认 pool 时,reactor 协程里 reload 仍成功(offload 退回 inline)。
static void testReloadInlineFallbackNoPool() {
    std::string cfg = writeCfg("nopool", "r1", "/v1", false);
    BxCpuPool::SetDefault(nullptr);   // 明确无池

    BxIoManager iom(2);
    {
        GatewayServer gw;
        gw.setCfgPath(cfg);
        std::atomic<bool> done{false};
        bool ok = false;
        size_t routeCnt = 0;

        iom.post([&]() {
            ok = gw.reload("");
            auto snap = gw.getConfig();
            routeCnt = (snap && snap->router) ? snap->router->routeCount() : 0;
            done = true;
        });

        TEST_CHECK_MSG(waitDone(done), "reload coroutine should finish");
        TEST_CHECK_MSG(ok, "reload should succeed even without CpuPool (inline fallback)");
        TEST_CHECK_MSG(routeCnt == 1, "snapshot swapped in inline fallback");
    }

    ::unlink(cfg.c_str());
    BRONX_LOG_INFO(bronx_test::logger()) << "done: testReloadInlineFallbackNoPool";
}

// 4) 主线程(无 fiber/iom)直接 reload 仍成功(老行为不回归)。
static void testReloadMainThreadNoContext() {
    std::string cfg = writeCfg("main", "r1", "/v1", false);
    BxCpuPool::SetDefault(nullptr);

    GatewayServer gw;
    gw.setCfgPath(cfg);
    bool ok = gw.reload("");                    // 主线程直接调,offload 见无 fiber → inline
    TEST_CHECK_MSG(ok, "main-thread reload should succeed (inline, old behavior intact)");
    auto snap = gw.getConfig();
    TEST_CHECK_MSG(snap && snap->router && snap->router->routeCount() == 1,
        "main-thread reload swapped snapshot");

    ::unlink(cfg.c_str());
    BRONX_LOG_INFO(bronx_test::logger()) << "done: testReloadMainThreadNoContext";
}

// 5) 压测:多协程并发 reload,狂切快照 + health timer arm/cancel churn。
//    竞态面:并发 offload 提交、eventfd 等待、m_cfgMtx 下换快照、旧快照析构取消 timer
//    与新快照装 timer 交错。配合 TSan 专门跑本用例。同时跑一个心跳协程,验证 reactor
//    在 reload 洪流下仍在推进(offload 把阻塞移出 reactor 的直接体现)。
static void testConcurrentReloadStress() {
    std::string cfg = writeCfg("stress", "r1", "/v1", /*healthCheck=*/true);
    auto pool = std::make_shared<BxCpuPool>(BxCpuPool::BxConfig{4, 0, "stress-cpu"});
    BxCpuPool::SetDefault(pool);

    const int kReloaders = 4;
    const int kPerReloader = 25;   // 共 100 次 reload

    BxIoManager iom(2);
    {
        GatewayServer gw;
        gw.setCfgPath(cfg);
        // 先装好初始快照,避免并发 reload 前 getConfig 为空
        gw.setConfig(GatewayConfig::BuildSnapshot(cfg));

        std::atomic<int> okCount{0};
        std::atomic<int> failCount{0};
        std::atomic<int> finished{0};
        std::atomic<uint64_t> heartbeat{0};
        std::atomic<bool> stopHeartbeat{false};

        // 心跳协程:hooked usleep 让出,reactor 每醒一次 +1。reload 洪流下仍应稳步推进。
        iom.post([&]() {
            while(!stopHeartbeat.load()) {
                heartbeat.fetch_add(1);
                usleep(2 * 1000);
            }
        });

        for(int t = 0; t < kReloaders; ++t) {
            iom.post([&]() {
                for(int i = 0; i < kPerReloader; ++i) {
                    if(gw.reload("")) okCount.fetch_add(1);
                    else              failCount.fetch_add(1);
                }
                finished.fetch_add(1);
            });
        }

        // 等所有 reloader 完成
        for(int i = 0; i < 1000 && finished.load() < kReloaders; ++i) {
            usleep(10 * 1000);
        }
        uint64_t hbBefore = heartbeat.load();
        usleep(50 * 1000);
        stopHeartbeat = true;

        TEST_CHECK_MSG(finished.load() == kReloaders, "all reloader coroutines finished");
        TEST_CHECK_MSG(okCount.load() == kReloaders * kPerReloader,
            "all concurrent reloads succeeded (got " + std::to_string(okCount.load()) + ")");
        TEST_CHECK_MSG(failCount.load() == 0, "no reload failed under concurrency");
        TEST_CHECK_MSG(heartbeat.load() > hbBefore,
            "heartbeat kept advancing during/after reload flood (reactor not starved)");
        auto snap = gw.getConfig();
        TEST_CHECK_MSG(snap && snap->router && snap->router->routeCount() == 1,
            "final snapshot valid after reload flood");
    }  // gw 析构在 iom 前

    BxCpuPool::SetDefault(nullptr);
    ::unlink(cfg.c_str());
    BRONX_LOG_INFO(bronx_test::logger()) << "done: testConcurrentReloadStress";
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    testReloadOffloadsToPool();
    testReloadBadPathKeepsOld();
    testReloadInlineFallbackNoPool();
    testReloadMainThreadNoContext();
    testConcurrentReloadStress();
    return TEST_SUMMARY();
}
