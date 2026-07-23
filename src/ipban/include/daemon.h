// Daemon 只管接入 登记 关停
#pragma once

#include "engine.h"
#include "session.h"
#include "db.h"
#include "cpu_pool.h"
#include "net_socket.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace bronx {
class BxIoManager;
class BxTimer;
namespace ipban {

struct DaemonOpts {
    std::string submitPath = "/tmp/bronx_ip_submit.sock";
    std::string subscribePath = "/tmp/bronx_ip_subscribe.sock";
    std::string adminPath;
    uint64_t expiryTickMs = 500;
    uint64_t helloTimeoutMs = 5000;
    uint64_t ackTimeoutMs = 60000;
    uint64_t stopTimeoutMs = 3000;
    bool sameUidOnly = true;
    std::string dbPath;
    uint64_t persistMinTtlMs = 300000;
};

struct LinkStat {
    uint64_t open = 0;
    uint64_t hello = 0;
    uint64_t sync = 0;
    uint64_t drain = 0;
    uint64_t accepts = 0;
    uint64_t closes = 0;
    uint64_t frameErr = 0;
    uint64_t msgErr = 0;
    uint64_t ackTimeout = 0;
    uint64_t pongTimeout = 0;
    uint64_t forced = 0;
    uint64_t peak = 0;
};

class Daemon {
public:
    Daemon(bronx::BxIoManager* iom, const DaemonOpts& opts);

    bool start();
    void beginStop();
    bool waitStop(uint64_t ms);
    void forceStop();
    void stop();

    uint64_t version() const { return m_engine.version(); }
    size_t activeRules() { return m_engine.ruleCount(); }
    size_t sessions() { return m_reg.size(); }
    size_t expiryQueue() { return m_engine.expiryQueue(); }
    LinkStat stats();
    HubStat hubStat();

private:
    enum class Side { SUBMIT, SUB, ADMIN };
    void acceptLoop(bronx::BxSocket::ptr lsock, Side side);
    void serveSubmit(bronx::BxSocket::ptr conn);
    void serveSubscribe(bronx::BxSocket::ptr conn);
    void serveAdmin(bronx::BxSocket::ptr conn);
    bronx::BxSocket::ptr listenUds(const std::string& path);
    void hold();
    void drop();
    void finishStop();
    void noteClose(const Session::ptr& sess);

    bronx::BxIoManager* m_iom;
    DaemonOpts m_opts;

    PolicyEngine m_engine;
    SessionRegistry m_reg;
    Broadcaster m_bcast{&m_reg};

    Db::ptr m_db;
    bronx::BxSocket::ptr m_submitSock;
    bronx::BxSocket::ptr m_subSock;
    bronx::BxSocket::ptr m_adminSock;
    std::mutex m_timerMtx;
    std::shared_ptr<bronx::BxTimer> m_expiryTimer;

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_stopped{false};
    std::atomic<bool> m_forceOnce{false};
    std::atomic<uint64_t> m_active{0};
    std::mutex m_waitMtx;
    std::condition_variable m_waitCv;
    std::mutex m_finishMtx;

    std::mutex m_editMtx;
    std::mutex m_reqMtx;
    std::unordered_set<bronx::BxSocket::ptr> m_reqConns;

    std::atomic<uint64_t> m_accepts{0};
    std::atomic<uint64_t> m_closes{0};
    std::atomic<uint64_t> m_frameErr{0};
    std::atomic<uint64_t> m_msgErr{0};
    std::atomic<uint64_t> m_ackTimeout{0};
    std::atomic<uint64_t> m_pongTimeout{0};
    std::atomic<uint64_t> m_forced{0};
    std::atomic<uint64_t> m_peak{0};
    std::array<std::atomic<uint64_t>, 3> m_links{};
};

} // namespace ipban
} // namespace bronx
