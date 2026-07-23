#include "db.h"
#include "wire.h"
#include "log.h"
#include <sqlite3.h>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace bronx {
namespace ipban {

static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("system");
static constexpr uint64_t kSqlIntMax = (uint64_t)std::numeric_limits<sqlite3_int64>::max();

static bool setPrivate(const std::string& path, bool missing = false) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd < 0) return missing && errno == ENOENT;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)
           && ::fchmod(fd, 0600) == 0;
    int err = ok ? 0 : errno;
    ::close(fd);
    if(!ok) errno = err ? err : EINVAL;
    return ok;
}

// 建表: rules 一行一条规则, body 是整条 JSON(复用 wire 的编码, 加字段不用改表)。
// ver/expire 单拎出来, 一个做版本守卫一个启动时过期过滤。meta 存 last_ver 一行。
static const char* kSchema =
    "CREATE TABLE IF NOT EXISTS rules("
    "  id TEXT PRIMARY KEY,"
    "  ver INTEGER NOT NULL CHECK(ver>=0),"
    "  expire INTEGER NOT NULL CHECK(expire>=0),"   // 0=永久
    "  body TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS tombstones("
    "  id TEXT PRIMARY KEY,"
    "  ver INTEGER NOT NULL CHECK(ver>=0));"
    "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v INTEGER NOT NULL CHECK(v>=0));"
    "INSERT OR IGNORE INTO meta(k,v) VALUES('last_ver',0);";

SqliteDb::ptr SqliteDb::open(const std::string& path) {
    ptr db(new SqliteDb());
    db->m_path = path;
    if(path.empty() || path.find('\0') != std::string::npos) return nullptr;
    if(path != ":memory:") {
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat st{};
        int err = fd < 0 ? errno : 0;
        bool ok = fd >= 0 && ::fstat(fd, &st) == 0;
        if(!ok && fd >= 0) err = errno;
        if(ok && !S_ISREG(st.st_mode)) {
            ok = false;
            err = EINVAL;
        }
        if(ok) ok = ::fchmod(fd, 0600) == 0;
        if(!ok && !err) err = errno;
        if(!ok) {
            BRONX_LOG_ERROR(g_log) << "sqlite private file failed path=" << path
                                   << " errno=" << err;
            if(fd >= 0) ::close(fd);
            return nullptr;
        }
        ::close(fd);
    }
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
              | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
    if(sqlite3_open_v2(path.c_str(), &db->m_db, flags, nullptr) != SQLITE_OK) {
        BRONX_LOG_ERROR(g_log) << "sqlite open failed path=" << path
                               << " err=" << sqlite3_errmsg(db->m_db);
        return nullptr;
    }
    // WAL: 写进日志就算持久, 读写不打架。NORMAL: 不每次 fsync, 崩溃只丢没提交的。
    // busy 超时防偶发锁竞争直接报错。
    if(!db->exec("PRAGMA journal_mode=WAL;") ||
       !db->exec("PRAGMA synchronous=NORMAL;") ||
       !db->exec("PRAGMA busy_timeout=3000;") ||
       !db->exec(kSchema)) {
        return nullptr;
    }
    if(path != ":memory:") {
        if(!setPrivate(path) || !setPrivate(path + "-wal", true)
           || !setPrivate(path + "-shm", true)) {
            BRONX_LOG_ERROR(g_log) << "sqlite chmod failed path=" << path
                                   << " errno=" << errno;
            return nullptr;
        }
    }
    BRONX_LOG_INFO(g_log) << "sqlite ready path=" << path;
    return db;
}

SqliteDb::~SqliteDb() {
    if(m_db) sqlite3_close(m_db);
}

bool SqliteDb::exec(const char* sql) {
    char* err = nullptr;
    if(sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        BRONX_LOG_ERROR(g_log) << "sqlite exec failed: " << (err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

// 一条 stmt 跑到底: prepare + 绑好 + step。绑参用回调, 省得每处重写。返回 step 是否 DONE。
static bool runStmt(sqlite3* db, const char* sql,
                    const std::function<void(sqlite3_stmt*)>& bind) {
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        BRONX_LOG_ERROR(g_log) << "sqlite prepare failed: " << sqlite3_errmsg(db);
        return false;
    }
    bind(st);
    int rc = sqlite3_step(st);
    bool ok = (rc == SQLITE_DONE);
    if(!ok) BRONX_LOG_ERROR(g_log) << "sqlite step failed rc=" << rc
                                   << " err=" << sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return ok;
}

// 上次事务没收干净(commit/rollback 失败会留着开着), 先回滚清掉。
// 不然下次 BEGIN 报 "transaction within a transaction", 此后写全静默失败。
void SqliteDb::healTxn() {
    if(!sqlite3_get_autocommit(m_db)) exec("ROLLBACK;");
}

bool SqliteDb::save(const Rule& r) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if(r.id.empty() || r.id.find('\0') != std::string::npos
       || r.version > kSqlIntMax || r.createdAtMs > kSqlIntMax
       || r.expireAtMs > kSqlIntMax) {
        BRONX_LOG_ERROR(g_log) << "sqlite save invalid rule";
        return false;
    }
    healTxn();
    // upsert 规则 + 抬 last_ver 两件事, 包一个显式事务一次提交, 少一次 WAL 写。
    // 版本守卫挡锁外并发落盘的乱序: 后到的旧版本别盖掉先落的新版本。
    if(!exec("BEGIN;")) return false;
    bool ok = runStmt(m_db, "DELETE FROM tombstones WHERE id=? AND ver<?;",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_text(st, 1, r.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)r.version);
        });
    std::string body = ruleToJson(r);
    ok = ok && runStmt(m_db,
        "INSERT INTO rules(id,ver,expire,body)"
        " SELECT ?,?,?,? WHERE NOT EXISTS("
        " SELECT 1 FROM tombstones WHERE id=? AND ver>=?)"
        " ON CONFLICT(id) DO UPDATE SET ver=excluded.ver,expire=excluded.expire,"
        " body=excluded.body WHERE excluded.ver>rules.ver;",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_text(st, 1, r.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)r.version);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)r.expireAtMs);
            sqlite3_bind_text(st, 4, body.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 5, r.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)r.version);
        });
    // last_ver = max(旧, 这版)
    ok = ok && runStmt(m_db, "UPDATE meta SET v=? WHERE k='last_ver' AND v<?;",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)r.version);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)r.version);
        });
    if(ok && exec("COMMIT;")) return true;
    exec("ROLLBACK;");   // commit 挂了也要回滚, 别把事务留着开
    return false;
}

bool SqliteDb::del(const std::string& ruleId, uint64_t ver) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if(ruleId.empty() || ruleId.find('\0') != std::string::npos || ver > kSqlIntMax) {
        BRONX_LOG_ERROR(g_log) << "sqlite del invalid id";
        return false;
    }
    healTxn();
    if(!exec("BEGIN;")) return false;
    // 留住删除版本, 旧 save 晚到也插不回来。
    bool ok = runStmt(m_db,
        "INSERT INTO tombstones(id,ver) VALUES(?,?)"
        " ON CONFLICT(id) DO UPDATE SET ver=excluded.ver"
        " WHERE excluded.ver>tombstones.ver;",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_text(st, 1, ruleId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)ver);
        });
    ok = ok && runStmt(m_db, "DELETE FROM rules WHERE id=? AND ver<=?;",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_text(st, 1, ruleId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)ver);
        });
    ok = ok && runStmt(m_db, "UPDATE meta SET v=? WHERE k='last_ver' AND v<?;",
        [&](sqlite3_stmt* st) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)ver);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)ver);
        });
    if(ok && exec("COMMIT;")) return true;
    exec("ROLLBACK;");
    return false;
}

bool SqliteDb::load(uint64_t nowMs, std::vector<Rule>& out, uint64_t& lastVer) {
    std::lock_guard<std::mutex> lk(m_mtx);
    out.clear();
    lastVer = 0;
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(m_db, "SELECT v FROM meta WHERE k='last_ver';", -1, &st, nullptr) != SQLITE_OK) {
        BRONX_LOG_ERROR(g_log) << "sqlite meta prepare failed: " << sqlite3_errmsg(m_db);
        return false;
    }
    int rc = sqlite3_step(st);
    if(rc != SQLITE_ROW || sqlite3_column_type(st, 0) != SQLITE_INTEGER
       || sqlite3_column_int64(st, 0) < 0) {
        BRONX_LOG_ERROR(g_log) << "sqlite meta invalid";
        sqlite3_finalize(st);
        return false;
    }
    lastVer = (uint64_t)sqlite3_column_int64(st, 0);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if(rc != SQLITE_DONE) {
        BRONX_LOG_ERROR(g_log) << "sqlite meta read stopped rc=" << rc
                               << " err=" << sqlite3_errmsg(m_db);
        return false;
    }
    bool clean = nowMs > kSqlIntMax
        ? exec("DELETE FROM rules WHERE expire != 0;")
        : runStmt(m_db, "DELETE FROM rules WHERE expire != 0 AND expire <= ?;",
            [&](sqlite3_stmt* s) { sqlite3_bind_int64(s, 1, (sqlite3_int64)nowMs); });
    if(!clean) {
        return false;
    }
    if(sqlite3_prepare_v2(m_db, "SELECT id,ver,expire,body FROM rules;", -1, &st, nullptr) != SQLITE_OK) {
        BRONX_LOG_ERROR(g_log) << "sqlite rules prepare failed: " << sqlite3_errmsg(m_db);
        return false;
    }
    bool bad = false;
    while((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* id = (const char*)sqlite3_column_text(st, 0);
        if(sqlite3_column_type(st, 1) != SQLITE_INTEGER
           || sqlite3_column_type(st, 2) != SQLITE_INTEGER
           || sqlite3_column_int64(st, 1) < 0 || sqlite3_column_int64(st, 2) < 0) {
            bad = true;
            break;
        }
        uint64_t ver = (uint64_t)sqlite3_column_int64(st, 1);
        uint64_t exp = (uint64_t)sqlite3_column_int64(st, 2);
        const char* body = (const char*)sqlite3_column_text(st, 3);
        if(!id || !*id || !body) {
            bad = true;
            break;
        }
        Rule r;
        if(!ruleFromJson(body, r) || r.id != id || r.version != ver || r.expireAtMs != exp) {
            bad = true;
            break;
        }
        if(ver > lastVer) lastVer = ver;
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    if(bad || rc != SQLITE_DONE) {
        out.clear();
        BRONX_LOG_ERROR(g_log) << "sqlite rules invalid rc=" << rc;
        return false;
    }
    return true;
}

} // namespace ipban
} // namespace bronx
