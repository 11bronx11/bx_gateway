#pragma once

// 规则落盘, daemon 重启不丢封禁。抽象接口留着方便测试和关持久化,
// 真家伙是 SqliteDb。热路径不碰这, 只有规则变(save/del)和启动 load 才走。
// 内存 store 才是查询源, 这里纯当冷备: 只写 + 启动读一次。

#include "rule.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace bronx {
namespace ipban {

class Db {
public:
    using ptr = std::shared_ptr<Db>;
    virtual ~Db() = default;

    // 存一条规则(带版本), 版本更旧就不覆盖。顺带把 last_ver 抬到这版。
    virtual bool save(const Rule& r) = 0;
    // 删一条(过期删), 顺带抬 last_ver。
    virtual bool del(const std::string& ruleId, uint64_t ver) = 0;
    // 启动读全量, 过滤掉 nowMs 已过期的。lastVer 回填库里记的最新版本。
    virtual bool load(uint64_t nowMs, std::vector<Rule>& out, uint64_t& lastVer) = 0;
};

// sqlite 实现。一张 rules 表(规则整条塞 JSON) + 一张 meta 存 last_ver。
// WAL 模式 + synchronous=NORMAL: 提交进 WAL 就算数, 崩溃只丢没提交的, 不用每次 fsync。
// 内部一把锁串行所有 db 调用, CPU 池多线程 offload 进来也安全。
class SqliteDb : public Db {
public:
    using ptr = std::shared_ptr<SqliteDb>;

    // 建/开库文件, 建表, 设 pragma。失败返回 nullptr。
    static ptr open(const std::string& path);
    ~SqliteDb() override;

    bool save(const Rule& r) override;
    bool del(const std::string& ruleId, uint64_t ver) override;
    bool load(uint64_t nowMs, std::vector<Rule>& out, uint64_t& lastVer) override;

private:
    SqliteDb() = default;
    bool exec(const char* sql);
    void healTxn();   // 上次没收干净的事务先回滚, 免得后续写全瘫

    sqlite3*   m_db = nullptr;
    std::string m_path;
    std::mutex  m_mtx;   // 串所有 db 调用, offload 来的多线程都过这把
};

} // namespace ipban
} // namespace bronx
