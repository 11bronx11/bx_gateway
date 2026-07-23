// 持久化压测: 在真 IoManager 协程里猛灌举报, 每条都走 offload 落盘(生产路径),
// 完了新开 engine 从同库 restore, 校验条数/版本一条不丢。压 offload 桥 + sqlite 并发写。
#include "test_util.h"
#include "engine.h"
#include "db.h"
#include "cpu_pool.h"
#include "reactor.h"
#include "log.h"
#include "util.h"
#include <atomic>
#include <string>
#include <unistd.h>

using namespace bronx::ipban;

static Ip makeIp(int a, int b, int c, int d) {
    Ip ip;
    parseCidr(std::to_string(a) + "." + std::to_string(b) + "."
        + std::to_string(c) + "." + std::to_string(d), ip);
    return ip;
}

int main(int argc, char**) {
    const int N = argc > 1 ? 200 : 2000;   // 带参跑轻量版
    auto sysLog = BRONX_LOG_NAME("system");
    auto old = sysLog->getLevel();
    sysLog->setLevel(bronx::BxLogLevel::ERROR);

    std::string dbPath = "/tmp/bronx_ipban_dbstress_" + std::to_string(::getpid()) + ".db";
    ::unlink(dbPath.c_str());
    ::unlink((dbPath + "-wal").c_str());
    ::unlink((dbPath + "-shm").c_str());

    // --- 阶段1: 协程里灌 N 条举报, 每条走 offload 落盘 ---
    {
        auto db = SqliteDb::open(dbPath);
        TEST_CHECK_MSG(db != nullptr, "open 应成功");
        bronx::BxCpuPool::BxConfig pc; pc.threads = 2; pc.name = "ipdbtest";
        auto pool = std::make_shared<bronx::BxCpuPool>(pc);

        PolicyEngine engine;
        engine.setDbOwned(db, pool);

        std::atomic<int> done{0};
        bronx::BxIoManager iom(2, "dbstress");
        // 分几个协程并发灌, 每个协程内 onRisk -> offload 落盘
        const int fibers = 4;
        for(int f = 0; f < fibers; ++f) {
            iom.post([&, f]() {
                for(int i = f; i < N; i += fibers) {
                    Risk r;
                    r.id = "s-" + std::to_string(i);
                    r.ip = makeIp(10, i / 65536, (i / 256) % 256, i % 256);
                    r.src = Src::RATE; r.banMs = 0;
                    r.atMs = bronx::GetCurrentMs();
                    engine.onRisk(r);
                }
                done.fetch_add(1);
            });
        }
        // 等所有灌完
        while(done.load() < fibers) usleep(20 * 1000);
        pool->drain();   // 等 offload 队列里的写全落完

        TEST_CHECK_EQ(engine.ruleCount(), (size_t)N);
        TEST_CHECK_EQ(engine.version(), (uint64_t)N);

        iom.stop();
    }

    // --- 阶段2: 新 engine 从同库 restore, 一条不丢 ---
    {
        auto db = SqliteDb::open(dbPath);
        PolicyEngine engine;
        engine.setDb(db, nullptr);   // restore 是同步读, 不用 pool
        engine.restore();
        TEST_CHECK_MSG(engine.ruleCount() == (size_t)N, "restore 后条数应对齐");
        TEST_CHECK_MSG(engine.version() == (uint64_t)N, "restore 后版本应接上不丢");
    }

    sysLog->setLevel(old);
    ::unlink(dbPath.c_str());
    ::unlink((dbPath + "-wal").c_str());
    ::unlink((dbPath + "-shm").c_str());
    return TEST_SUMMARY();
}
