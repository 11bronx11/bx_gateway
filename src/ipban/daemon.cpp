#include "daemon.h"
#include "proto.h"
#include "wire.h"
#include "reactor.h"
#include "endpoint.h"
#include "log.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace bronx {
namespace ipban {

static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("system");

static bool peerAllowed(const bronx::BxSocket::ptr& conn) {
    struct ucred cr{};
    socklen_t len = sizeof(cr);
    if(getsockopt(conn->getSocket(), SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) {
        BRONX_LOG_WARN(g_log) << "peercred get failed errno=" << errno;
        return false;
    }
    uid_t me = ::geteuid();
    if(cr.uid == me || cr.uid == 0) return true;
    BRONX_LOG_WARN(g_log) << "reject peer uid=" << cr.uid << " pid=" << cr.pid
                          << " want=" << me;
    return false;
}

Daemon::Daemon(bronx::BxIoManager* iom, const DaemonOpts& opts)
    : m_iom(iom), m_opts(opts) {
}

static bool clearSocketFile(const std::string& path) {
    if(path.empty()) return false;
    struct stat st{};
    if(::lstat(path.c_str(), &st) != 0) {
        if(errno == ENOENT) return true;
        BRONX_LOG_ERROR(g_log) << "uds lstat failed path=" << path << " errno=" << errno;
        return false;
    }
    if(!S_ISSOCK(st.st_mode)) {
        BRONX_LOG_ERROR(g_log) << "uds path is not socket path=" << path;
        return false;
    }
    if(::unlink(path.c_str()) == 0) return true;
    BRONX_LOG_ERROR(g_log) << "uds unlink failed path=" << path << " errno=" << errno;
    return false;
}

bronx::BxSocket::ptr Daemon::listenUds(const std::string& path) {
    if(!clearSocketFile(path)) return nullptr;
    auto sock = bronx::BxSocket::MakeUnixTcpSocket();
    auto addr = bronx::BxUnixAddress::Create(path);
    if(!sock || !addr) {
        BRONX_LOG_ERROR(g_log) << "uds make failed path=" << path;
        return nullptr;
    }
    if(!sock->bind(addr)) {
        BRONX_LOG_ERROR(g_log) << "uds bind failed path=" << path
                               << " errno=" << errno << " " << strerror(errno);
        sock->close();
        clearSocketFile(path);
        return nullptr;
    }
    if(::chmod(path.c_str(), 0600) != 0) {
        BRONX_LOG_ERROR(g_log) << "uds chmod failed path=" << path
                               << " errno=" << errno << " " << strerror(errno);
        sock->close();
        clearSocketFile(path);
        return nullptr;
    }
    if(!sock->listen()) {
        BRONX_LOG_ERROR(g_log) << "uds listen failed path=" << path;
        sock->close();
        clearSocketFile(path);
        return nullptr;
    }
    return sock;
}

bool Daemon::start() {
    if(!m_iom || !m_opts.expiryTickMs || !m_opts.helloTimeoutMs
       || !m_opts.ackTimeoutMs || !m_opts.stopTimeoutMs
       || m_opts.submitPath.empty() || m_opts.subscribePath.empty()
       || m_opts.submitPath == m_opts.subscribePath
       || (!m_opts.adminPath.empty() && (m_opts.submitPath == m_opts.adminPath
           || m_opts.subscribePath == m_opts.adminPath)) || m_started.load()) {
        BRONX_LOG_ERROR(g_log) << "ip_policy_daemon bad config";
        return false;
    }

    if(!m_opts.dbPath.empty()) {
        m_db = SqliteDb::open(m_opts.dbPath);
        if(!m_db) return false;
        m_engine.setDb(m_db, BxCpuPool::GetDefault(), m_opts.persistMinTtlMs);
        if(!m_engine.restore()) {
            m_engine.setDb(nullptr, nullptr);
            m_db.reset();
            return false;
        }
    }

    m_submitSock = listenUds(m_opts.submitPath);
    m_subSock = listenUds(m_opts.subscribePath);
    if(!m_opts.adminPath.empty()) m_adminSock = listenUds(m_opts.adminPath);
    if(!m_submitSock || !m_subSock || (!m_opts.adminPath.empty() && !m_adminSock)) {
        if(m_submitSock) {
            m_submitSock->close();
            clearSocketFile(m_opts.submitPath);
        }
        if(m_subSock) {
            m_subSock->close();
            clearSocketFile(m_opts.subscribePath);
        }
        if(m_adminSock) {
            m_adminSock->close();
            clearSocketFile(m_opts.adminPath);
        }
        m_engine.setDb(nullptr, nullptr);
        m_db.reset();
        return false;
    }

    m_engine.setOnChange([this](uint64_t ver) { m_bcast.publish(ver); });
    m_started.store(true, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    hold();
    hold();
    m_iom->post([this]() { acceptLoop(m_submitSock, Side::SUBMIT); });
    m_iom->post([this]() { acceptLoop(m_subSock, Side::SUB); });
    if(m_adminSock) {
        hold();
        m_iom->post([this]() { acceptLoop(m_adminSock, Side::ADMIN); });
    }
    {
        std::lock_guard<std::mutex> lk(m_timerMtx);
        m_expiryTimer = m_iom->addTimer(m_opts.expiryTickMs, [this]() {
            std::lock_guard<std::mutex> lk(m_editMtx);
            if(m_running.load(std::memory_order_acquire)) m_engine.tickExpiry();
        }, true);
    }
    BRONX_LOG_INFO(g_log) << "ip_policy_daemon up submit=" << m_opts.submitPath
                          << " subscribe=" << m_opts.subscribePath
                          << " admin=" << (m_opts.adminPath.empty() ? "off" : m_opts.adminPath);
    return true;
}

void Daemon::hold() {
    m_active.fetch_add(1, std::memory_order_acq_rel);
}

void Daemon::drop() {
    if(m_active.fetch_sub(1, std::memory_order_acq_rel) == 1
       && m_stopping.load(std::memory_order_acquire)) finishStop();
}

void Daemon::beginStop() {
    {
        std::lock_guard<std::mutex> lk(m_editMtx);
        bool expected = false;
        if(!m_stopping.compare_exchange_strong(expected, true)) return;
        m_running.store(false, std::memory_order_release);
    }
    if(!m_started.load(std::memory_order_acquire)) {
        m_stopped.store(true, std::memory_order_release);
        m_waitCv.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_timerMtx);
        if(m_expiryTimer) m_expiryTimer->cancel();
        m_expiryTimer.reset();
    }
    if(m_submitSock) m_submitSock->close();
    if(m_subSock) m_subSock->close();
    if(m_adminSock) m_adminSock->close();
    m_reg.stopAll();
    if(m_active.load(std::memory_order_acquire) == 0) finishStop();
}

bool Daemon::waitStop(uint64_t ms) {
    std::unique_lock<std::mutex> lk(m_waitMtx);
    return m_waitCv.wait_for(lk, std::chrono::milliseconds(ms), [this]() {
        return m_stopped.load(std::memory_order_acquire);
    });
}

void Daemon::forceStop() {
    beginStop();
    bool expected = false;
    if(!m_forceOnce.compare_exchange_strong(expected, true)) return;
    std::vector<bronx::BxSocket::ptr> conns;
    {
        std::lock_guard<std::mutex> lk(m_reqMtx);
        conns.assign(m_reqConns.begin(), m_reqConns.end());
    }
    m_forced.fetch_add(conns.size(), std::memory_order_relaxed);
    for(auto& conn : conns) conn->close();
    m_reg.forceAll();
}

void Daemon::stop() {
    beginStop();
    if(bronx::BxIoManager::Current() == m_iom) return;
    if(waitStop(m_opts.stopTimeoutMs)) return;
    forceStop();
    waitStop(m_opts.stopTimeoutMs);
}

void Daemon::finishStop() {
    std::lock_guard<std::mutex> lk(m_finishMtx);
    if(m_stopped.load(std::memory_order_acquire)) return;
    m_engine.setOnChange(nullptr);
    if(!m_engine.flushPersistence()) {
        BRONX_LOG_WARN(g_log) << "ipban stop with pending db ops="
                              << m_engine.pendingPersistence();
    }
    m_engine.setDb(nullptr, nullptr);
    m_db.reset();
    clearSocketFile(m_opts.submitPath);
    clearSocketFile(m_opts.subscribePath);
    clearSocketFile(m_opts.adminPath);
    m_stopped.store(true, std::memory_order_release);
    m_waitCv.notify_all();
    LinkStat s = stats();
    BRONX_LOG_INFO(g_log) << "ipban stopped accepts=" << s.accepts
                          << " closes=" << s.closes
                          << " peak=" << s.peak
                          << " frame_err=" << s.frameErr
                          << " msg_err=" << s.msgErr
                          << " ack_timeout=" << s.ackTimeout
                          << " pong_timeout=" << s.pongTimeout
                          << " forced=" << s.forced;
}

void Daemon::acceptLoop(bronx::BxSocket::ptr lsock, Side side) {
    while(m_running.load(std::memory_order_acquire)) {
        auto conn = lsock->accept();
        if(!conn) {
            if(errno == ECANCELED || errno == EBADF || !m_running.load()) break;
            usleep(10000);
            continue;
        }
        if(!m_running.load(std::memory_order_acquire)) {
            conn->close();
            break;
        }
        if(m_opts.sameUidOnly && !peerAllowed(conn)) {
            conn->close();
            continue;
        }
        if(side == Side::SUB) {
            serveSubscribe(conn);
        } else {
            hold();
            if(side == Side::ADMIN) {
                m_iom->post([this, conn]() { serveAdmin(conn); });
            } else {
                m_iom->post([this, conn]() { serveSubmit(conn); });
            }
        }
    }
    drop();
}

void Daemon::serveSubmit(bronx::BxSocket::ptr conn) {
    {
        std::lock_guard<std::mutex> lk(m_reqMtx);
        m_reqConns.insert(conn);
    }
    while(true) {
        if(!m_running.load(std::memory_order_acquire)) break;
        Msg msg;
        ProtoErr err;
        MsgRet ret = recvMsg(conn, msg, &err);
        if(ret != MsgRet::OK) break;
        if(msg.kind != Kind::RISK) break;
        Risk risk;
        if(!riskFromJson(msg.body, risk)) break;
        std::lock_guard<std::mutex> lk(m_editMtx);
        if(!m_running.load(std::memory_order_acquire)) break;
        m_engine.onRisk(risk);
    }
    {
        std::lock_guard<std::mutex> lk(m_reqMtx);
        m_reqConns.erase(conn);
    }
    conn->close();
    drop();
}

void Daemon::serveAdmin(bronx::BxSocket::ptr conn) {
    {
        std::lock_guard<std::mutex> lk(m_reqMtx);
        m_reqConns.insert(conn);
    }

    Msg msg;
    CtlRes res;
    ProtoErr err;
    bool got = false;
    if(m_running.load(std::memory_order_acquire)
       && recvMsg(conn, msg, &err) == MsgRet::OK) {
        got = true;
        std::lock_guard<std::mutex> lk(m_editMtx);
        if(!m_running.load(std::memory_order_acquire)) {
            res.error = "stopping";
        } else if(msg.kind == Kind::PUT) {
            CtlPut put;
            if(!ctlPutFromJson(msg.body, put)) {
                res.error = "bad_put";
            } else {
                EditRes edit = m_engine.setAdmin(put.ip, put.action, put.ttlMs, put.reason);
                res.ok = edit.error.empty() && edit.durable;
                res.changed = edit.changed;
                res.version = edit.version;
                res.id = edit.id;
                res.error = edit.error;
            }
        } else if(msg.kind == Kind::DEL) {
            std::string id;
            if(!ctlDelFromJson(msg.body, id)) {
                res.error = "bad_del";
            } else {
                EditRes edit = m_engine.remove(id);
                res.ok = edit.error.empty() && edit.durable;
                res.changed = edit.changed;
                res.version = edit.version;
                res.id = edit.id;
                res.error = edit.error;
            }
        } else if(msg.kind == Kind::LIST && msg.body.empty()) {
            res.ok = true;
            res.version = m_engine.version();
            res.rules = m_engine.rules();
            std::sort(res.rules.begin(), res.rules.end(), [](const Rule& a, const Rule& b) {
                return a.id < b.id;
            });
        } else {
            res.error = "bad_kind";
        }
    }

    if(got) sendMsg(conn, Kind::RESULT, ctlResToJson(res), msg.seq);
    {
        std::lock_guard<std::mutex> lk(m_reqMtx);
        m_reqConns.erase(conn);
    }
    conn->close();
    drop();
}

void Daemon::serveSubscribe(bronx::BxSocket::ptr conn) {
    m_accepts.fetch_add(1, std::memory_order_relaxed);
    auto pushFn = [this](uint64_t sentVer, bool sentOnce,
                         Kind& kind, std::string& body, uint64_t& newVer) {
        return m_engine.nextPush(sentVer, sentOnce, kind, body, newVer);
    };
    auto verFn = [this]() { return m_engine.version(); };
    auto readyFn = [this](const Session::ptr& sess) {
        for(auto& old : m_reg.bind(sess)) {
            old->beginStop(Session::StopWhy::REPLACED, false);
        }
    };
    auto doneFn = [this](const Session::ptr& sess) {
        noteClose(sess);
        m_reg.remove(sess);
        drop();
    };
    auto sess = std::make_shared<Session>(conn, m_engine.epoch(), verFn, pushFn,
                                          readyFn, doneFn,
                                          m_opts.helloTimeoutMs, m_opts.ackTimeoutMs);
    hold();         // 先 hold，再 add；否则 beginStop/stopAll 可能在 add 后 hold 前触发，
    m_reg.add(sess);// doneFn 调 drop() 时 m_active 还没加，导致永不归零
    uint64_t size = m_reg.size();
    uint64_t peak = m_peak.load(std::memory_order_relaxed);
    while(peak < size && !m_peak.compare_exchange_weak(peak, size)) {
    }
    sess->start(m_iom);
    if(!m_running.load(std::memory_order_acquire)) {
        sess->beginStop(Session::StopWhy::LOCAL, false);
    }
}

void Daemon::noteClose(const Session::ptr& sess) {
    m_closes.fetch_add(1, std::memory_order_relaxed);
    switch(sess->stopWhy()) {
        case Session::StopWhy::NONE:
        case Session::StopWhy::LOCAL:
        case Session::StopWhy::PEER:
            m_links[0].fetch_add(1, std::memory_order_relaxed);
            break;
        case Session::StopWhy::REPLACED:
            m_links[1].fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            m_links[2].fetch_add(1, std::memory_order_relaxed);
            break;
    }
    if(sess->frameErr() != FrameErr::NONE) m_frameErr.fetch_add(1, std::memory_order_relaxed);
    if(sess->msgErr() != MsgErr::NONE) m_msgErr.fetch_add(1, std::memory_order_relaxed);
    if(sess->stopWhy() == Session::StopWhy::ACK_TIMEOUT)
        m_ackTimeout.fetch_add(1, std::memory_order_relaxed);
    if(sess->stopWhy() == Session::StopWhy::PONG_TIMEOUT)
        m_pongTimeout.fetch_add(1, std::memory_order_relaxed);
    if(sess->forced()) m_forced.fetch_add(1, std::memory_order_relaxed);
}

LinkStat Daemon::stats() {
    LinkStat out;
    for(const auto& sess : m_reg.list()) {
        switch(sess->state()) {
            case Session::State::NEW:
            case Session::State::HELLO: ++out.hello; break;
            case Session::State::SYNC: ++out.sync; break;
            case Session::State::OPEN: ++out.open; break;
            case Session::State::DRAIN: ++out.drain; break;
            case Session::State::CLOSING:
            case Session::State::CLOSED: break;
        }
    }
    out.accepts = m_accepts.load(std::memory_order_relaxed);
    out.closes = m_closes.load(std::memory_order_relaxed);
    out.frameErr = m_frameErr.load(std::memory_order_relaxed);
    out.msgErr = m_msgErr.load(std::memory_order_relaxed);
    out.ackTimeout = m_ackTimeout.load(std::memory_order_relaxed);
    out.pongTimeout = m_pongTimeout.load(std::memory_order_relaxed);
    out.forced = m_forced.load(std::memory_order_relaxed);
    out.peak = m_peak.load(std::memory_order_relaxed);
    return out;
}

HubStat Daemon::hubStat() {
    HubStat out = m_engine.hubStat();
    out.up = m_running.load(std::memory_order_acquire);
    out.sessions = m_reg.size();
    out.sessionsPeak = m_peak.load(std::memory_order_relaxed);
    for(size_t i = 0; i < out.links.size(); ++i)
        out.links[i] = m_links[i].load(std::memory_order_relaxed);
    LinkStat link = stats();
    out.linkErr[0] = link.frameErr;
    out.linkErr[1] = link.msgErr;
    out.linkErr[2] = link.ackTimeout;
    out.linkErr[3] = link.pongTimeout;
    out.linkErr[4] = link.forced;
    return out;
}

} // namespace ipban
} // namespace bronx
