#include "pull.h"
#include "proto.h"
#include "wire.h"
#include "reactor.h"
#include "endpoint.h"
#include "metrics.h"
#include "util.h"
#include "log.h"
#include <algorithm>
#include <chrono>
#include <vector>

namespace bronx {
namespace ipban {

static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("system");

static uint64_t stateNum(SyncClient::State state) {
    switch(state) {
        case SyncClient::State::DOWN: return 0;
        case SyncClient::State::DIAL: return 1;
        case SyncClient::State::HELLO: return 2;
        case SyncClient::State::SYNC: return 3;
        case SyncClient::State::OPEN: return 4;
        case SyncClient::State::BACKOFF: return 5;
        case SyncClient::State::STOPPING: return 6;
        case SyncClient::State::STOPPED: return 7;
    }
    return 0;
}

void SyncClient::setState(State state) {
    m_state.store(state, std::memory_order_release);
    auto& metrics = gateway::GatewayMetrics::instance();
    metrics.set_ipban_link_state(stateNum(state), m_metricId);
    bool open = state == State::OPEN;
    metrics.set_ipban_link_up(open, m_metricId);
    metrics.set_ipban_synced(open, m_metricId);
}

void SyncClient::start(bronx::BxIoManager* iom) {
    bool expected = false;
    if(!iom || !m_guard || m_opts.subscribePath.empty() || !m_opts.minBackoffMs
       || m_opts.maxBackoffMs < m_opts.minBackoffMs || !m_opts.recvTimeoutMs
       || !m_opts.stopTimeoutMs || !m_started.compare_exchange_strong(expected, true)) return;
    m_iom = iom;
    m_metricId = gateway::GatewayMetrics::instance().claim_ipban();
    m_running.store(true, std::memory_order_release);
    m_guard->setRemoteEnabled(true);
    auto self = shared_from_this();
    if(m_opts.expiryTickMs) {
        std::weak_ptr<SyncClient> weak = self;
        std::lock_guard<std::mutex> lk(m_timerMtx);
        m_expiryTimer = iom->addTimer(m_opts.expiryTickMs, [weak]() {
            if(auto sync = weak.lock()) {
                sync->m_guard->tickExpiry();
                sync->updateAges();
            }
        }, true);
    }
    iom->post([self]() { self->loop(); });
}

void SyncClient::stop() {
    if(!m_started.load(std::memory_order_acquire)) return;
    bool was = m_running.exchange(false, std::memory_order_acq_rel);
    if(was) setState(State::STOPPING);
    {
        std::lock_guard<std::mutex> lk(m_timerMtx);
        if(m_expiryTimer) m_expiryTimer->cancel();
        if(m_backoffTimer) m_backoffTimer->cancel();
        m_expiryTimer.reset();
        m_backoffTimer.reset();
    }
    m_wait.notify();
    bronx::BxSocket::ptr sock;
    {
        std::lock_guard<std::mutex> lk(m_sockMtx);
        sock = m_sock;
    }
    if(sock) sock->close();
    if(bronx::BxIoManager::Current() != m_iom && !waitStop(m_opts.stopTimeoutMs)) {
        m_stopForce.store(true, std::memory_order_release);
    }
}

bool SyncClient::waitStop(uint64_t ms) {
    std::unique_lock<std::mutex> lk(m_doneMtx);
    return m_doneCv.wait_for(lk, std::chrono::milliseconds(ms), [this]() {
        return m_done.load(std::memory_order_acquire);
    });
}

bool SyncClient::sendOne(const bronx::BxSocket::ptr& sock, Kind kind,
                         const std::string& body, uint64_t seq) {
    ProtoErr err;
    if(!sendMsg(sock, kind, body, seq, &err)) {
        if(err.frame != FrameErr::NONE)
            gateway::GatewayMetrics::instance().incr_ipban_frame_err();
        return false;
    }
    gateway::GatewayMetrics::instance().incr_ipban_tx();
    return true;
}

bool SyncClient::waitBackoff(uint64_t ms) {
    std::weak_ptr<SyncClient> weak = shared_from_this();
    auto timer = m_iom->addTimer(ms, [weak]() {
        if(auto sync = weak.lock()) sync->m_wait.notify();
    });
    {
        std::lock_guard<std::mutex> lk(m_timerMtx);
        if(!m_running.load(std::memory_order_acquire)) {
            timer->cancel();
            return false;
        }
        m_backoffTimer = timer;
    }
    m_wait.wait();
    {
        std::lock_guard<std::mutex> lk(m_timerMtx);
        timer->cancel();
        if(m_backoffTimer == timer) m_backoffTimer.reset();
    }
    return m_running.load(std::memory_order_acquire);
}

void SyncClient::updateAges() {
    uint64_t now = bronx::GetCurrentMs();
    uint64_t rx = m_lastRx.load(std::memory_order_relaxed);
    uint64_t ack = m_lastAck.load(std::memory_order_relaxed);
    auto& metrics = gateway::GatewayMetrics::instance();
    metrics.set_ipban_rx_idle(rx && now >= rx ? now - rx : 0, m_metricId);
    metrics.set_ipban_ack_idle(ack && now >= ack ? now - ack : 0, m_metricId);
    uint64_t target = m_targetVer.load(std::memory_order_relaxed);
    uint64_t local = m_guard->remoteVersion();
    metrics.set_ipban_sync_lag(target > local ? target - local : 0, m_metricId);
}

void SyncClient::loop() {
    uint64_t backoff = m_opts.minBackoffMs;
    bool tried = false;
    while(m_running.load(std::memory_order_acquire)) {
        if(tried) gateway::GatewayMetrics::instance().incr_ipban_reconnect();
        tried = true;
        bool fast = false;
        bool opened = session(fast);
        setState(State::DOWN);
        if(!m_running.load(std::memory_order_acquire)) break;
        if(fast) {
            backoff = m_opts.minBackoffMs;
            continue;
        }
        backoff = opened ? m_opts.minBackoffMs
                         : std::min(backoff * 2, m_opts.maxBackoffMs);
        setState(State::BACKOFF);
        if(!waitBackoff(backoff)) break;
    }
    finish();
}

bool SyncClient::session(bool& fast) {
    setState(State::DIAL);
    auto sock = bronx::BxSocket::MakeUnixTcpSocket();
    auto addr = bronx::BxUnixAddress::Create(m_opts.subscribePath);
    if(!sock || !addr) return false;
    {
        std::lock_guard<std::mutex> lk(m_sockMtx);
        m_sock = sock;
    }
    auto clearSock = [this, &sock]() {
        {
            std::lock_guard<std::mutex> lk(m_sockMtx);
            if(m_sock == sock) m_sock.reset();
        }
        sock->close();
    };
    if(!sock->connect(addr, 2000)) {
        clearSock();
        return false;
    }
    gateway::GatewayMetrics::instance().incr_ipban_connect();
    setState(State::HELLO);
    uint64_t helloSeq = m_nextSeq++;
    if(!sendOne(sock, Kind::HELLO,
                helloToJson(m_opts.instanceId, m_guard->remoteEpoch(), m_guard->remoteVersion()),
                helloSeq)) {
        clearSock();
        return false;
    }

    sock->setRecvTimeout((int64_t)m_opts.recvTimeoutMs);
    Msg ready;
    ProtoErr err;
    MsgRet ret = recvMsg(sock, ready, &err);
    if(ret != MsgRet::OK) {
        if(err.frame != FrameErr::NONE)
            gateway::GatewayMetrics::instance().incr_ipban_frame_err();
        clearSock();
        return false;
    }
    auto& metrics = gateway::GatewayMetrics::instance();
    metrics.incr_ipban_rx();
    m_lastRx.store(bronx::GetCurrentMs(), std::memory_order_relaxed);
    uint64_t epoch = 0;
    uint64_t target = 0;
    if(ready.kind != Kind::READY || ready.seq != helloSeq
       || !readyFromJson(ready.body, epoch, target)) {
        metrics.incr_ipban_msg_err();
        clearSock();
        return false;
    }
    m_targetVer.store(target, std::memory_order_relaxed);
    bool same = epoch == m_guard->remoteEpoch() && target == m_guard->remoteVersion();
    if(same) {
        setState(State::OPEN);
        updateAges();
    } else {
        setState(State::SYNC);
    }

    bool opened = same;
    unsigned badSync = 0;
    while(m_running.load(std::memory_order_acquire)) {
        Msg msg;
        err = {};
        ret = recvMsg(sock, msg, &err);
        if(ret != MsgRet::OK) {
            if(err.frame != FrameErr::NONE) metrics.incr_ipban_frame_err();
            break;
        }
        metrics.incr_ipban_rx();
        uint64_t now = bronx::GetCurrentMs();
        m_lastRx.store(now, std::memory_order_relaxed);
        metrics.set_ipban_rx_idle(0, m_metricId);

        if(msg.kind == Kind::BYE) {
            if(msg.seq || !msg.body.empty()) {
                metrics.incr_ipban_msg_err();
                break;
            }
            fast = m_running.load(std::memory_order_acquire);
            break;
        }
        if(msg.kind == Kind::ERR) {
            if(msg.seq) {
                metrics.incr_ipban_msg_err();
                break;
            }
            std::string code;
            if(!errFromJson(msg.body, code)) {
                metrics.incr_ipban_msg_err();
            } else if(code == "ack_timeout") {
                metrics.incr_ipban_ack_timeout();
            } else if(code == "pong_timeout") {
                metrics.incr_ipban_pong_timeout();
            }
            break;
        }
        if(msg.kind == Kind::PING) {
            if(!msg.seq || !msg.body.empty()) {
                metrics.incr_ipban_msg_err();
                break;
            }
            uint64_t pingAt = bronx::GetCurrentMs();
            if(!sendOne(sock, Kind::PONG, "", msg.seq)) break;
            metrics.set_ipban_ping_ms(bronx::GetCurrentMs() - pingAt, m_metricId);
            continue;
        }
        if((msg.kind != Kind::SNAP && msg.kind != Kind::DELTA) || !msg.seq) {
            metrics.incr_ipban_msg_err();
            break;
        }

        setState(State::SYNC);
        bool delta = msg.kind == Kind::DELTA;
        bool applied = false;
        size_t why = 0;
        uint64_t ver = 0;
        if(msg.kind == Kind::SNAP) {
            uint64_t gotEpoch = 0;
            std::vector<Rule> rules;
            if(!snapFromJson(msg.body, gotEpoch, ver, rules)) {
                why = 0;
            } else if(gotEpoch != epoch) {
                why = 1;
            } else {
                uint64_t oldEpoch = m_guard->remoteEpoch();
                applied = m_guard->applyRemote(gotEpoch, ver, std::move(rules));
                if(!applied) {
                    why = 2;
                } else if(oldEpoch && oldEpoch != gotEpoch) {
                    metrics.incr_epoch_change();
                }
            }
        } else {
            uint64_t gotEpoch = 0;
            uint64_t prev = 0;
            std::vector<DeltaOp> ops;
            if(!deltaFromJson(msg.body, gotEpoch, prev, ver, ops)) {
                why = 0;
            } else if(gotEpoch != epoch) {
                why = 1;
            } else {
                applied = m_guard->applyDelta(gotEpoch, prev, ver, ops);
                if(!applied) why = 2;
            }
        }
        metrics.note_sync_apply(delta, applied);

        if(!applied) {
            metrics.incr_ipban_msg_err();
            metrics.note_sync_resync(why);
            if(++badSync > 1 || !sendOne(sock, Kind::SNAP_REQ, "")) break;
            continue;
        }
        badSync = 0;
        m_targetVer.store(ver, std::memory_order_relaxed);
        metrics.set_ipban_version(m_guard->remoteVersion(), m_metricId);
        if(!sendOne(sock, Kind::ACK,
                    helloToJson(m_opts.instanceId, m_guard->remoteEpoch(),
                                m_guard->remoteVersion()), msg.seq)) break;
        m_lastAck.store(bronx::GetCurrentMs(), std::memory_order_relaxed);
        metrics.set_ipban_ack_idle(0, m_metricId);
        metrics.set_ipban_sync_lag(0, m_metricId);
        setState(State::OPEN);
        opened = true;
    }

    clearSock();
    return opened;
}

void SyncClient::finish() {
    setState(State::STOPPED);
    auto& metrics = gateway::GatewayMetrics::instance();
    if(m_stopForce.load(std::memory_order_acquire)) metrics.incr_ipban_stop_force();
    else metrics.incr_ipban_stop_grace();
    m_done.store(true, std::memory_order_release);
    m_doneCv.notify_all();
    BRONX_LOG_INFO(g_log) << "ipban sync stopped";
}

} // namespace ipban
} // namespace bronx
