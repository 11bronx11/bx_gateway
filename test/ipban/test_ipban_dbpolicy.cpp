// 选择性落盘策略: 只落"值得重启恢复"的封 —— 永久 or 原始时长>=阈值。
// 短临时封不落盘(重启窗口内多半已过期, 落了白落)。验证:
//  1. 永久封 + 长封 -> restore 后还在
//  2. 短封 -> restore 后没了(压根没写库)
//  3. 阈值边界
// pool=nullptr 时 offload 退化 inline 同步, 灌完立即可查库, 不用等异步。
#include "test_util.h"
#include "engine.h"
#include "db.h"
#include "rule.h"
#include "ip.h"
#include "util.h"
#include <limits>
#include <string>
#include <unistd.h>

using namespace bronx::ipban;

static Ip mkIp(const char* s) { Ip ip; parseCidr(s, ip); return ip; }

static Risk mkRisk(const std::string& id, const char* ip, uint64_t banMs) {
    Risk r; r.id = id; r.ip = mkIp(ip); r.src = Src::RATE; r.banMs = banMs;
    r.atMs = bronx::GetCurrentMs(); r.reason = "policy test";
    return r;
}

class FlakyDb : public Db {
public:
    bool failSave = true;
    bool failDel = false;
    bool has = false;
    Rule row;
    uint64_t lastVer = 0;

    bool save(const Rule& r) override {
        if(failSave) return false;
        if(!has || r.version > row.version) {
            row = r;
            has = true;
        }
        if(r.version > lastVer) lastVer = r.version;
        return true;
    }

    bool del(const std::string& id, uint64_t ver) override {
        if(failDel) return false;
        if(has && row.id == id && row.version < ver) has = false;
        if(ver > lastVer) lastVer = ver;
        return true;
    }

    bool load(uint64_t now, std::vector<Rule>& out, uint64_t& ver) override {
        out.clear();
        if(has && (row.expireAtMs == 0 || row.expireAtMs > now)) out.push_back(row);
        ver = lastVer;
        return true;
    }
};

// 新 engine 从库 restore, 数数恢复出多少条、某 id 在不在
static size_t restoreCount(const std::string& path, uint64_t minTtl) {
    auto db = SqliteDb::open(path);
    PolicyEngine e;
    e.setDb(db, nullptr, minTtl);
    e.restore();
    return e.ruleCount();
}

int main() {
    std::string path = "/tmp/bronx_ipban_dbpolicy_" + std::to_string(::getpid()) + ".db";
    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());

    const uint64_t minTtl = 60000;   // 阈值 60s: 短于此不落

    // --- 灌不同时长的封 ---
    {
        auto db = SqliteDb::open(path);
        TEST_CHECK(db != nullptr);
        PolicyEngine e;
        e.setDb(db, nullptr, minTtl);   // pool=null -> inline 同步落盘

        e.onRisk(mkRisk("perm",  "1.1.1.1", 0));         // 永久 -> 落
        e.onRisk(mkRisk("long",  "2.2.2.2", 120000));    // 2min >= 阈值 -> 落
        e.onRisk(mkRisk("short", "3.3.3.3", 500));       // 0.5s < 阈值 -> 不落
        e.onRisk(mkRisk("edge",  "4.4.4.4", 60000));     // 正好 =阈值 -> 落(>=)

        // 内存里 4 条都在(内存不受落盘策略影响, 权威照旧)
        TEST_CHECK_MSG(e.ruleCount() == 4, "内存里应有全部 4 条");
    }

    // --- restore: 只应恢复 perm/long/edge 三条, short 没落盘 ---
    {
        size_t n = restoreCount(path, minTtl);
        TEST_CHECK_MSG(n == 3, "restore 应只恢复落过盘的 3 条(短封没落)");
    }

    // --- 直接读库确认 short 真的不在, perm 在 ---
    {
        auto db = SqliteDb::open(path);
        std::vector<Rule> out; uint64_t lastVer = 0;
        db->load(bronx::GetCurrentMs(), out, lastVer);
        bool hasShort = false, hasPerm = false, hasEdge = false;
        for(const auto& r : out) {
            if(r.id == "rate:3.3.3.3") hasShort = true;
            if(r.id == "rate:1.1.1.1") hasPerm = true;
            if(r.id == "rate:4.4.4.4") hasEdge = true;
        }
        TEST_CHECK_MSG(!hasShort, "短封不该在库里");
        TEST_CHECK_MSG(hasPerm, "永久封应在库里");
        TEST_CHECK_MSG(hasEdge, "正好等于阈值的应在(>=)");
    }

    // --- minTtl=0 时全落(关闭选择性) ---
    {
        std::string p2 = "/tmp/bronx_ipban_dbpolicy2_" + std::to_string(::getpid()) + ".db";
        ::unlink(p2.c_str());
        auto db = SqliteDb::open(p2);
        PolicyEngine e;
        e.setDb(db, nullptr, 0);        // 0 = 全落
        e.onRisk(mkRisk("s1", "5.5.5.5", 100));   // 极短也落
        e.onRisk(mkRisk("s2", "6.6.6.6", 200));
        size_t n = restoreCount(p2, 0);
        TEST_CHECK_MSG(n == 2, "minTtl=0 应全落, restore 出 2 条");
        ::unlink(p2.c_str());
        ::unlink((p2 + "-wal").c_str());
        ::unlink((p2 + "-shm").c_str());
    }

    // --- 写删失败都保留原 op, tick 后按顺序重试 ---
    {
        auto db = std::make_shared<FlakyDb>();
        PolicyEngine e;
        e.setDb(db, nullptr, 0);
        TEST_CHECK(e.onRisk(mkRisk("retry", "7.7.7.7", 0)));
        TEST_CHECK_MSG(!e.persistenceHealthy() && e.pendingPersistence() == 1,
                       "save 失败应留下待重试 op");
        TEST_CHECK(!db->has);

        db->failSave = false;
        e.tickExpiry();
        TEST_CHECK(e.persistenceHealthy() && e.pendingPersistence() == 0);
        TEST_CHECK(db->has);

        db->failDel = true;
        EditRes del = e.remove("rate:7.7.7.7");
        TEST_CHECK(del.changed && !del.durable && del.error == "persist_failed");
        TEST_CHECK(e.pendingPersistence() == 1 && db->has);

        db->failDel = false;
        e.tickExpiry();
        TEST_CHECK(e.persistenceHealthy() && e.pendingPersistence() == 0);
        TEST_CHECK_MSG(!db->has, "del 失败恢复后不能让规则复活");
    }

    // --- 提高阈值后, restore 出来的旧规则仍要从库删除 ---
    {
        std::string p3 = "/tmp/bronx_ipban_dbpolicy3_" + std::to_string(::getpid()) + ".db";
        ::unlink(p3.c_str());
        {
            auto db = SqliteDb::open(p3);
            PolicyEngine e;
            e.setDb(db, nullptr, 0);
            TEST_CHECK(e.onRisk(mkRisk("old-short", "8.8.8.8", 120000)));
        }
        {
            auto db = SqliteDb::open(p3);
            PolicyEngine e;
            e.setDb(db, nullptr, 600000);
            TEST_CHECK(e.restore());
            EditRes del = e.remove("rate:8.8.8.8");
            TEST_CHECK(del.changed && del.durable);
            std::vector<Rule> out;
            uint64_t ver = 0;
            TEST_CHECK(db->load(bronx::GetCurrentMs(), out, ver));
            TEST_CHECK_MSG(out.empty(), "阈值变化后删除不能漏库");
        }
        ::unlink(p3.c_str());
        ::unlink((p3 + "-wal").c_str());
        ::unlink((p3 + "-shm").c_str());
    }

    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());
    return TEST_SUMMARY();
}
