// SqliteDb 持久化: 存取往返 / 版本守卫(旧不覆盖新) / 过期过滤 / 关了重开还在 /
// last_ver 恢复 / 并发写(多线程都过内部锁)。这是持久化的地基回归守卫。
#include "test_util.h"
#include "db.h"
#include "engine.h"
#include "rule.h"
#include "ip.h"
#include "util.h"
#include <sqlite3.h>
#include <limits>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace bronx::ipban;

static Ip mkIp(const char* s) { Ip ip; parseCidr(s, ip); return ip; }

static Rule mkRule(const std::string& id, const char* ip, uint64_t ver,
                   uint64_t expire, Act act = Act::DENY) {
    Rule r;
    r.id = id; r.ip = mkIp(ip); r.action = act; r.src = Src::RATE;
    r.priority = rulePriority(r.src, r.action);
    r.createdAtMs = bronx::GetCurrentMs();
    r.expireAtMs = expire; r.version = ver; r.reason = "db test";
    return r;
}

static const Rule* findById(const std::vector<Rule>& v, const std::string& id) {
    for(const auto& r : v) if(r.id == id) return &r;
    return nullptr;
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
    std::string path = "/tmp/bronx_ipban_db_" + std::to_string(::getpid()) + ".db";
    ::unlink(path.c_str());
    // WAL 会带 -wal/-shm 副本, 一并清
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());

    uint64_t now = bronx::GetCurrentMs();

    // --- 1. 基本存取往返 ---
    {
        auto db = SqliteDb::open(path);
        TEST_CHECK_MSG(db != nullptr, "open 应成功");
        struct stat st{};
        TEST_CHECK(::stat(path.c_str(), &st) == 0);
        TEST_CHECK_EQ(st.st_mode & 0777, 0600u);
        TEST_CHECK(::stat((path + "-wal").c_str(), &st) == 0);
        TEST_CHECK_EQ(st.st_mode & 0777, 0600u);
        TEST_CHECK(::stat((path + "-shm").c_str(), &st) == 0);
        TEST_CHECK_EQ(st.st_mode & 0777, 0600u);
        TEST_CHECK(db->save(mkRule("r1", "1.2.3.4", 1, 0)));           // 永久
        TEST_CHECK(db->save(mkRule("r2", "10.0.0.0/8", 2, now + 100000)));  // 网段 + 未过期
        std::vector<Rule> out; uint64_t lastVer = 0;
        TEST_CHECK(db->load(now, out, lastVer));
        TEST_CHECK_EQ(out.size(), (size_t)2);
        TEST_CHECK_EQ(lastVer, (uint64_t)2);
        const Rule* r1 = findById(out, "r1");
        TEST_CHECK_MSG(r1 && r1->ip == mkIp("1.2.3.4"), "r1 ip 应往返一致");
        TEST_CHECK_MSG(r1 && r1->action == Act::DENY, "r1 action 应往返");
    }

    // SQLite INTEGER 是有符号 64 位, 超界值必须在写入前挡掉。
    {
        auto db = SqliteDb::open(path);
        Rule bad = mkRule("too-big", "6.6.6.6", 22,
                          std::numeric_limits<uint64_t>::max());
        TEST_CHECK_MSG(!db->save(bad), "超大 expire 不能静默写成负数");
    }

    {
        std::string edgePath = "/tmp/bronx_ipban_db_time_" + std::to_string(::getpid()) + ".db";
        ::unlink(edgePath.c_str());
        auto db = SqliteDb::open(edgePath);
        TEST_CHECK(db->save(mkRule("edge-time", "6.6.6.6", 23,
                                  (uint64_t)std::numeric_limits<int64_t>::max())));
        std::vector<Rule> out;
        uint64_t lastVer = 0;
        TEST_CHECK(db->load(std::numeric_limits<uint64_t>::max(), out, lastVer));
        TEST_CHECK_MSG(out.empty(), "超大 now 应过滤临时规则");
        ::unlink(edgePath.c_str());
        ::unlink((edgePath + "-wal").c_str());
        ::unlink((edgePath + "-shm").c_str());
    }

    // --- 2. 关了重开, 数据还在(核心: 持久化) ---
    {
        auto db = SqliteDb::open(path);
        std::vector<Rule> out; uint64_t lastVer = 0;
        TEST_CHECK(db->load(now, out, lastVer));
        TEST_CHECK_MSG(out.size() == 2, "重开后规则应还在");
        TEST_CHECK_EQ(lastVer, (uint64_t)2);
    }

    // 提交后直接退, WAL 还能恢复
    {
        std::string crashPath = "/tmp/bronx_ipban_db_crash_" + std::to_string(::getpid()) + ".db";
        ::unlink(crashPath.c_str());
        pid_t pid = ::fork();
        TEST_CHECK(pid >= 0);
        if(pid == 0) {
            auto db = SqliteDb::open(crashPath);
            bool ok = db && db->save(mkRule("crash", "4.4.4.4", 3, 0));
            ::_exit(ok ? 0 : 1);
        }
        int status = 0;
        TEST_CHECK(::waitpid(pid, &status, 0) == pid);
        TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        auto db = SqliteDb::open(crashPath);
        std::vector<Rule> out;
        uint64_t lastVer = 0;
        TEST_CHECK(db && db->load(now, out, lastVer));
        TEST_CHECK_MSG(findById(out, "crash") != nullptr, "异常退出后规则应还在");
        db.reset();
        ::unlink(crashPath.c_str());
        ::unlink((crashPath + "-wal").c_str());
        ::unlink((crashPath + "-shm").c_str());
    }

    // --- 3. 版本守卫: 旧版本不覆盖新版本 ---
    {
        auto db = SqliteDb::open(path);
        db->save(mkRule("r1", "9.9.9.9", 10, 0));   // r1 升到 v10, ip 改了
        db->save(mkRule("r1", "1.2.3.4", 5, 0));    // 旧 v5 想盖, 应被守卫挡下
        std::vector<Rule> out; uint64_t lastVer = 0;
        db->load(now, out, lastVer);
        const Rule* r1 = findById(out, "r1");
        TEST_CHECK_MSG(r1 && r1->ip == mkIp("9.9.9.9"), "旧版本不该盖掉新版本");
        TEST_CHECK_MSG(lastVer >= 10, "last_ver 应抬到 10 不回退");
    }

    // --- 4. 过期过滤: load 时 expire<=now 的不收 ---
    {
        auto db = SqliteDb::open(path);
        db->save(mkRule("gone", "8.8.8.8", 20, now - 1));      // 已过期
        db->save(mkRule("live", "7.7.7.7", 21, now + 100000)); // 没过期
        std::vector<Rule> out; uint64_t lastVer = 0;
        db->load(now, out, lastVer);
        TEST_CHECK_MSG(findById(out, "gone") == nullptr, "过期规则不该 load 出来");
        TEST_CHECK_MSG(findById(out, "live") != nullptr, "未过期规则应在");
    }

    // --- 5. del 删掉 ---
    {
        auto db = SqliteDb::open(path);
        db->del("r1", 30);
        std::vector<Rule> out; uint64_t lastVer = 0;
        db->load(now, out, lastVer);
        TEST_CHECK_MSG(findById(out, "r1") == nullptr, "del 后不该在");
        TEST_CHECK_MSG(lastVer >= 30, "del 也抬 last_ver");
    }

    // --- 5b. del 版本守卫: 旧 del 别抹掉后落的新封禁(offload 乱序) ---
    {
        std::string rpath = "/tmp/bronx_ipban_db_reorder_" + std::to_string(::getpid()) + ".db";
        ::unlink(rpath.c_str());
        auto db = SqliteDb::open(rpath);
        // 同一 id: 先落新封禁 v7, 再来个旧到期删 op=5(v7 是 del 之后重新封的, 版本更高)
        db->save(mkRule("x", "5.5.5.5", 7, now + 100000));
        db->del("x", 5);
        std::vector<Rule> out; uint64_t lastVer = 0;
        db->load(now, out, lastVer);
        TEST_CHECK_MSG(findById(out, "x") != nullptr, "旧 del 不该抹掉后落的新封禁");
        // 反向: del op 更新则照删
        db->del("x", 9);
        db->load(now, out, lastVer);
        TEST_CHECK_MSG(findById(out, "x") == nullptr, "更新的 del 应删掉");

        // 删除先落, 旧 save 晚到也不能复活
        db->save(mkRule("x", "5.5.5.5", 7, now + 100000));
        db->load(now, out, lastVer);
        TEST_CHECK_MSG(findById(out, "x") == nullptr, "旧 save 不该穿过墓碑");

        // 真正的新版本可以重新写入
        db->save(mkRule("x", "5.5.5.5", 10, now + 100000));
        db->load(now, out, lastVer);
        TEST_CHECK_MSG(findById(out, "x") != nullptr, "新版本应能重新写入");

        db->del("x", 10);
        db->load(now, out, lastVer);
        TEST_CHECK_MSG(findById(out, "x") == nullptr, "同版本删除应生效");
        ::unlink(rpath.c_str());
        ::unlink((rpath + "-wal").c_str());
        ::unlink((rpath + "-shm").c_str());
    }

    // 坏输入和链接路径
    {
        auto db = SqliteDb::open(path);
        Rule bad = mkRule(std::string("bad\0id", 6), "6.6.6.6", 31, 0);
        TEST_CHECK_MSG(!db->save(bad), "带空字节的 id 不能落库");

        std::string target = "/tmp/bronx_ipban_db_target_" + std::to_string(::getpid()) + ".db";
        std::string link = target + ".link";
        ::unlink(target.c_str());
        ::unlink(link.c_str());
        auto targetDb = SqliteDb::open(target);
        TEST_CHECK(targetDb != nullptr);
        targetDb.reset();
        TEST_CHECK(::symlink(target.c_str(), link.c_str()) == 0);
        TEST_CHECK_MSG(SqliteDb::open(link) == nullptr, "db 路径不能跟符号链接");
        ::unlink(link.c_str());
        ::unlink(target.c_str());
        ::unlink((target + "-wal").c_str());
        ::unlink((target + "-shm").c_str());
    }

    // --- 6. 坏库不能只恢复半截 ---
    {
        std::string bpath = "/tmp/bronx_ipban_db_bad_" + std::to_string(::getpid()) + ".db";
        ::unlink(bpath.c_str());
        auto db = SqliteDb::open(bpath);
        TEST_CHECK(db->save(mkRule("bad", "6.6.6.6", 1, 0)));
        TEST_CHECK(rawSql(bpath, "PRAGMA ignore_check_constraints=ON;"
                                 "UPDATE meta SET v=-1 WHERE k='last_ver';"));
        std::vector<Rule> out; uint64_t lastVer = 0;
        TEST_CHECK_MSG(!db->load(now, out, lastVer), "坏 meta 不能当成功");
        PolicyEngine engine;
        engine.setDb(db, nullptr);
        TEST_CHECK_MSG(!engine.restore(), "坏库不能空着启动");
        TEST_CHECK(rawSql(bpath, "UPDATE meta SET v=1 WHERE k='last_ver';"));
        TEST_CHECK(rawSql(bpath, "UPDATE rules SET body='{}';"));
        TEST_CHECK_MSG(!db->load(now, out, lastVer), "坏规则不能漏着恢复");
        ::unlink(bpath.c_str());
        ::unlink((bpath + "-wal").c_str());
        ::unlink((bpath + "-shm").c_str());
    }

    // 写锁释放后能重试
    {
        std::string lockPath = "/tmp/bronx_ipban_db_lock_" + std::to_string(::getpid()) + ".db";
        ::unlink(lockPath.c_str());
        auto db = SqliteDb::open(lockPath);
        sqlite3* hold = nullptr;
        TEST_CHECK(sqlite3_open(lockPath.c_str(), &hold) == SQLITE_OK);
        TEST_CHECK(sqlite3_exec(hold, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK);
        TEST_CHECK_MSG(!db->save(mkRule("locked", "3.3.3.3", 32, 0)), "写锁占着应失败");
        TEST_CHECK(sqlite3_exec(hold, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK);
        TEST_CHECK(db->save(mkRule("locked", "3.3.3.3", 32, 0)));
        std::vector<Rule> out;
        uint64_t lastVer = 0;
        TEST_CHECK(db->load(now, out, lastVer));
        TEST_CHECK_MSG(findById(out, "locked") != nullptr, "写锁释放后应恢复");
        sqlite3_close(hold);
        db.reset();
        ::unlink(lockPath.c_str());
        ::unlink((lockPath + "-wal").c_str());
        ::unlink((lockPath + "-shm").c_str());
    }

    // --- 7. 并发写: 多线程 save 都过内部锁, 不崩不丢 ---
    {
        std::string cpath = "/tmp/bronx_ipban_db_cc_" + std::to_string(::getpid()) + ".db";
        ::unlink(cpath.c_str());
        auto db = SqliteDb::open(cpath);
        const int T = 4, N = 100;
        std::vector<std::thread> ths;
        for(int t = 0; t < T; ++t) {
            ths.emplace_back([&, t]() {
                for(int i = 0; i < N; ++i) {
                    std::string id = "c" + std::to_string(t) + "_" + std::to_string(i);
                    db->save(mkRule(id, "1.1.1.1", (uint64_t)(t * N + i + 1), 0));
                }
            });
        }
        for(auto& th : ths) th.join();
        std::vector<Rule> out; uint64_t lastVer = 0;
        db->load(now, out, lastVer);
        TEST_CHECK_MSG(out.size() == (size_t)(T * N), "并发写应全部落库无丢");
        ::unlink(cpath.c_str());
        ::unlink((cpath + "-wal").c_str());
        ::unlink((cpath + "-shm").c_str());
    }

    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());
    return TEST_SUMMARY();
}
