// 持久化端到端: daemon 挂 db 封 IP -> 落盘 -> daemon 重启灌回 -> 网关重连仍 DENY。
// 这是"重启不丢封禁"的回归守卫, 补上 test_ipban_restart(那个测的是无持久化清掉)。
// 对比点: 无 db 重启后放行(旧行为), 有 db 重启后仍封(新行为)。
#include "test_util.h"
#include "daemon.h"
#include "pull.h"
#include "report.h"
#include "guard.h"
#include "cpu_pool.h"
#include "reactor.h"
#include "util.h"
#include <atomic>
#include <sqlite3.h>
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

// 等 guard 的 epoch 变成新实例的(证明真收到了 daemon2 的全量, 不是抱着旧的)
static bool waitEpochChange(Guard::ptr g, uint64_t oldEpoch, uint64_t timeoutMs) {
    uint64_t deadline = bronx::GetCurrentMs() + timeoutMs;
    while(bronx::GetCurrentMs() < deadline) {
        if(g->remoteEpoch() != oldEpoch) return true;
        usleep(50 * 1000);
    }
    return g->remoteEpoch() != oldEpoch;
}

static bool rawSql(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    if(sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        if(db) sqlite3_close(db);
        return false;
    }
    char* err = nullptr;
    bool ok = sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK;
    sqlite3_free(err);
    sqlite3_close(db);
    return ok;
}

int main() {
    TEST_CHECK_MSG(bronx::BxCpuPool::GetDefaultPtr() == nullptr,
                   "持久化 daemon 不依赖应用默认 CPU pool");

    std::string base = "/tmp/bronx_ipban_persist_" + std::to_string(::getpid());
    std::string submitPath = base + "_submit.sock";
    std::string subPath    = base + "_sub.sock";
    std::string dbPath     = base + ".db";
    ::unlink(dbPath.c_str());
    ::unlink((dbPath + "-wal").c_str());
    ::unlink((dbPath + "-shm").c_str());

    DaemonOpts dopts;
    dopts.submitPath = submitPath;
    dopts.subscribePath = subPath;
    dopts.expiryTickMs = 100;
    dopts.dbPath = dbPath;               // 挂持久化

    SyncOpts sopts; sopts.subscribePath = subPath; sopts.instanceId = "gw-persist";
    ReportOpts ropts; ropts.submitPath = submitPath;

    auto guard = std::make_shared<Guard>();
    guard->setEnabled(true);
    bronx::BxIoManager iomG(1, "gw");
    auto sync = std::make_shared<SyncClient>(guard, sopts);
    auto reporter = std::make_shared<Reporter>(ropts);

    Ip bad = mkIp("1.2.3.4");

    // --- daemon1 挂 db, 封 1.2.3.4 永久 ---
    auto iomD1 = std::make_shared<bronx::BxIoManager>(1, "d1");
    auto d1 = std::make_shared<Daemon>(iomD1.get(), dopts);
    iomD1->post([&]() { if(!d1->start()) TEST_FAIL("d1 start failed"); });

    iomG.post([&]() {
        sync->start(&iomG);
        reporter->start(&iomG);
        usleep(800 * 1000);
        Risk r; r.id="pst-1"; r.ip=bad; r.src=Src::RATE; r.banMs=0;
        r.atMs=bronx::GetCurrentMs(); r.reason="persist test";
        reporter->tryReport(r);
    });

    TEST_CHECK_MSG(waitAction(guard, bad, Act::DENY, 4000), "daemon1 封后应 DENY");
    uint64_t ep1 = guard->remoteEpoch();
    usleep(500 * 1000);   // 留时间让 offload 落盘

    // --- daemon1 干净停机(drain 落盘), 进程级挂掉 ---
    d1->stop();
    iomD1->stop();
    d1.reset();
    iomD1.reset();
    usleep(400 * 1000);

    // 网关此刻应还抱着旧 DENY(daemon 没了, guard 不清)
    TEST_CHECK_MSG(guard->eval(bad).action == Act::DENY, "daemon 停了网关仍抱旧规则");

    // --- daemon2 起在同 socket + 同 db(新 epoch, 但规则从库灌回) ---
    auto iomD2 = std::make_shared<bronx::BxIoManager>(1, "d2");
    auto d2 = std::make_shared<Daemon>(iomD2.get(), dopts);
    iomD2->post([&]() { if(!d2->start()) TEST_FAIL("d2 start failed"); });

    // 先等 guard 换上 daemon2 的新 epoch(证明真收到了重启后的全量, 而非抱着旧的)
    TEST_CHECK_MSG(waitEpochChange(guard, ep1, 8000), "网关应收到 daemon2 新 epoch 的全量");
    // 新 epoch 的全量里带着从库灌回的封禁 -> 仍 DENY(有持久化的核心区别)
    TEST_CHECK_MSG(waitAction(guard, bad, Act::DENY, 3000),
                   "有持久化时 daemon 重启后旧封禁应还在");
    TEST_CHECK_MSG(d2->activeRules() >= 1, "daemon2 应从库恢复出规则");

    sync->stop();
    reporter->stop();
    usleep(200 * 1000);
    d2->stop();
    iomD2->stop();

    // 坏库不能起 daemon
    TEST_CHECK(rawSql(dbPath, "UPDATE rules SET body='{}';"));
    auto iomD3 = std::make_shared<bronx::BxIoManager>(1, "d3");
    auto d3 = std::make_shared<Daemon>(iomD3.get(), dopts);
    std::atomic<bool> done{false};
    std::atomic<bool> started{true};
    iomD3->post([&]() {
        started.store(d3->start(), std::memory_order_release);
        done.store(true, std::memory_order_release);
    });
    uint64_t deadline = bronx::GetCurrentMs() + 3000;
    while(!done.load(std::memory_order_acquire) && bronx::GetCurrentMs() < deadline) {
        usleep(10 * 1000);
    }
    TEST_CHECK_MSG(done.load(std::memory_order_acquire) && !started.load(std::memory_order_acquire),
                   "坏库不该启动");
    d3->stop();
    iomD3->stop();

    iomG.stop();
    ::unlink(submitPath.c_str());
    ::unlink(subPath.c_str());
    ::unlink(dbPath.c_str());
    ::unlink((dbPath + "-wal").c_str());
    ::unlink((dbPath + "-shm").c_str());
    return TEST_SUMMARY();
}
