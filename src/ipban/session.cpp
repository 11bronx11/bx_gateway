#include "session.h"
#include "wire.h"
#include "reactor.h"
#include "util.h"
#include "log.h"
#include <chrono>
#include <sstream>

namespace bronx {
namespace ipban {

static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("system");
static std::atomic<uint64_t> g_conn{1};

static const char* stateName(Session::State s) {
    switch(s) {
        case Session::State::NEW: return "new";
        case Session::State::HELLO: return "hello";
        case Session::State::SYNC: return "sync";
        case Session::State::OPEN: return "open";
        case Session::State::DRAIN: return "drain";
        case Session::State::CLOSING: return "closing";
        case Session::State::CLOSED: return "closed";
    }
    return "unknown";
}

static const char* whyName(Session::StopWhy why) {
    switch(why) {
        case Session::StopWhy::NONE: return "none";
        case Session::StopWhy::LOCAL: return "local";
        case Session::StopWhy::PEER: return "peer";
        case Session::StopWhy::IO: return "io";
        case Session::StopWhy::FRAME: return "frame";
        case Session::StopWhy::MSG: return "msg";
        case Session::StopWhy::ACK_TIMEOUT: return "ack_timeout";
        case Session::StopWhy::PONG_TIMEOUT: return "pong_timeout";
        case Session::StopWhy::REPLACED: return "replaced";
        case Session::StopWhy::STOP_TIMEOUT: return "stop_timeout";
    }
    return "unknown";
}

Session::Session(bronx::BxSocket::ptr sock, uint64_t epoch, VerFn verFn, PushFn pushFn,
                 ReadyFn readyFn, DoneFn doneFn, uint64_t helloMs, uint64_t ackMs)
    : m_sock(std::move(sock))
    , m_epoch(epoch)
    , m_verFn(std::move(verFn))
    , m_pushFn(std::move(pushFn))
    , m_readyFn(std::move(readyFn))
    , m_doneFn(std::move(doneFn))
    , m_helloMs(helloMs)
    , m_ackMs(ackMs)
    , m_id(g_conn.fetch_add(1, std::memory_order_relaxed)) {
}

void Session::start(bronx::BxIoManager* iom) {
    if(!iom || !m_sock) {
        beginStop(StopWhy::IO, false);
        setState(State::CLOSED);
        return;
    }
    m_startedAt = bronx::GetCurrentMs();
    m_loops.store(2, std::memory_order_release);
    setState(State::HELLO);
    auto self = shared_from_this();
    iom->post([self]() { self->recvLoop(); });
    iom->post([self]() { self->sendLoop(); });
}

void Session::setState(State state) {
    m_state.store(state, std::memory_order_release);
}

void Session::wake() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(state() >= State::DRAIN) return;
        m_pushDue = true;
        if(m_sync.seq) m_dirty = true;
    }
    m_waker.notify();
}

void Session::beginStop(StopWhy why, bool drain) {
    bool close = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        State cur = state();
        if(cur == State::CLOSED) return;
        if(m_why == StopWhy::NONE) m_why = why;
        if(drain && cur >= State::SYNC && cur < State::DRAIN) {
            setState(State::DRAIN);
        } else if(cur < State::CLOSING) {
            setState(State::CLOSING);
            close = true;
        }
    }
    m_waker.notify();
    if(close) closeNow();
}

bool Session::waitStop(uint64_t ms) {
    std::unique_lock<std::mutex> lk(m_mtx);
    return m_cv.wait_for(lk, std::chrono::milliseconds(ms), [this]() {
        return state() == State::CLOSED;
    });
}

void Session::forceStop() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(state() == State::CLOSED) return;
        if(m_why == StopWhy::NONE) m_why = StopWhy::STOP_TIMEOUT;
        m_forced = true;
        setState(State::CLOSING);
    }
    closeNow();
}

Session::StopWhy Session::stopWhy() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_why;
}

bool Session::forced() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_forced;
}

std::string Session::instanceId() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_inst;
}

FrameErr Session::frameErr() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_protoErr.frame;
}

MsgErr Session::msgErr() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_msgErr;
}

IoErr Session::ioErr() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_protoErr.io;
}

uint64_t Session::aliveMs() const {
    uint64_t now = bronx::GetCurrentMs();
    return m_startedAt && now >= m_startedAt ? now - m_startedAt : 0;
}

bool Session::sendOne(Kind kind, const std::string& body, uint64_t seq) {
    ProtoErr err;
    if(!sendMsg(m_sock, kind, body, seq, &err)) {
        failProto(err);
        return false;
    }
    m_tx.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool Session::sendReady() {
    uint64_t seq = 0;
    uint64_t target = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(!m_ready) return true;
        m_ready = false;
        seq = m_helloSeq;
        target = m_targetVer;
    }
    if(!sendOne(Kind::READY, readyToJson(m_epoch, target), seq)) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(state() >= State::DRAIN) return true;
        if(m_sentOnce && m_sentVer == target) {
            setState(State::OPEN);
        } else {
            setState(State::SYNC);
            m_pushDue = true;
        }
    }
    return true;
}

bool Session::sendCurrent() {
    uint64_t from = 0;
    bool sentOnce = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(state() >= State::DRAIN) return true;
        if(m_sync.seq) {
            if(m_pushDue) m_dirty = true;
            m_pushDue = false;
            return true;
        }
        if(!m_pushDue && !m_forceFull) return true;
        if(m_forceFull) {
            m_sentOnce = false;
            m_forceFull = false;
        }
        from = m_sentVer;
        sentOnce = m_sentOnce;
        m_pushDue = false;
    }

    Kind kind = Kind::SNAP;
    std::string body;
    uint64_t ver = 0;
    if(!m_pushFn(from, sentOnce, kind, body, ver)) {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(state() < State::DRAIN) setState(State::OPEN);
        return true;
    }

    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        seq = m_nextSeq++;
        m_sync = {seq, ver, UINT64_MAX};
        m_sent.store(ver, std::memory_order_release);
        m_dirty = m_dirty || m_pushDue;
        m_pushDue = false;
        setState(State::SYNC);
    }
    if(!sendOne(kind, body, seq)) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(m_sync.seq == seq) m_sync.due = bronx::GetCurrentMs() + m_ackMs;
        m_sentVer = ver;
        m_sentOnce = true;
    }
    return true;
}

bool Session::sendPing() {
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(!m_pingDue || m_ping.seq || m_sync.seq || state() >= State::DRAIN) return true;
        m_pingDue = false;
        seq = m_nextSeq++;
        m_ping = {seq, UINT64_MAX};
    }
    if(!sendOne(Kind::PING, "", seq)) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(m_ping.seq == seq) m_ping.due = bronx::GetCurrentMs() + m_ackMs;
    }
    return true;
}

void Session::sendLoop() {
    while(state() != State::CLOSED) {
        if(!m_waker.wait()) break;

        std::string sendErr;
        State cur;
        bool ready = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            cur = state();
            sendErr.swap(m_sendErr);
            ready = m_ready;
        }
        if(!sendErr.empty()) {
            sendOne(Kind::ERR, errToJson(sendErr), 0);
            closeNow();
            break;
        }
        if(cur == State::CLOSING || cur == State::CLOSED) break;
        if(ready && !sendReady()) break;
        if(!sendCurrent()) break;
        if(!sendPing()) break;

        bool bye = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if(state() == State::DRAIN && m_sync.seq == 0) {
                setState(State::CLOSING);
                bye = true;
            }
        }
        if(bye) {
            sendOne(Kind::BYE, "", 0);
            closeNow();
            break;
        }
    }
    closeNow();
    loopDone();
}

void Session::onMsg(const Msg& msg) {
    if(msg.kind == Kind::ACK) {
        std::string inst;
        uint64_t epoch = 0;
        uint64_t ver = 0;
        if(!helloFromJson(msg.body, inst, epoch, ver)) {
            failMsg(MsgErr::BAD_JSON, "bad_ack");
            return;
        }
        bool wakeSend = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if(!m_sync.seq || msg.seq != m_sync.seq || inst != m_inst
               || epoch != m_epoch || ver != m_sync.ver) {
                wakeSend = false;
            } else {
                m_acked.store(ver, std::memory_order_release);
                m_sync = {};
                if(state() < State::DRAIN) setState(State::OPEN);
                if(m_dirty) {
                    m_dirty = false;
                    m_pushDue = true;
                }
                wakeSend = true;
            }
        }
        if(!wakeSend) {
            failMsg(MsgErr::BAD_ACK, "bad_ack");
            return;
        }
        m_waker.notify();
        return;
    }

    if(msg.kind == Kind::SNAP_REQ) {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            ok = !msg.seq && msg.body.empty() && state() == State::SYNC;
            if(ok) {
                m_sync = {};
                m_forceFull = true;
                m_pushDue = true;
            }
        }
        if(!ok) {
            failMsg(MsgErr::BAD_STATE, "bad_state");
            return;
        }
        m_waker.notify();
        return;
    }

    if(msg.kind == Kind::PONG) {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            ok = m_ping.seq && msg.seq == m_ping.seq && msg.body.empty();
            if(ok) m_ping = {};
        }
        if(!ok) failMsg(MsgErr::BAD_PONG, "bad_pong");
        return;
    }

    failMsg(MsgErr::BAD_STATE, "bad_state");
}

void Session::onTimeout() {
    std::string code;
    StopWhy why = StopWhy::NONE;
    bool wakeSend = false;
    uint64_t now = bronx::GetCurrentMs();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(m_sync.seq && now >= m_sync.due) {
            code = "ack_timeout";
            why = StopWhy::ACK_TIMEOUT;
        } else if(m_ping.seq && now >= m_ping.due) {
            code = "pong_timeout";
            why = StopWhy::PONG_TIMEOUT;
        } else if(state() == State::DRAIN) {
            wakeSend = true;
        } else if(!m_sync.seq && !m_ping.seq) {
            m_pingDue = true;
            wakeSend = true;
        } else {
            return;
        }
        if(wakeSend) {
            code.clear();
        } else {
        if(m_why == StopWhy::NONE) m_why = why;
        m_sendErr = code;
        setState(State::CLOSING);
        }
    }
    m_waker.notify();
}

void Session::recvLoop() {
    m_sock->setRecvTimeout((int64_t)m_helloMs);
    Msg hello;
    ProtoErr err;
    MsgRet ret = recvMsg(m_sock, hello, &err);
    if(ret != MsgRet::OK) {
        failProto(err);
        loopDone();
        return;
    }
    m_rx.fetch_add(1, std::memory_order_relaxed);
    std::string inst;
    uint64_t peerEpoch = 0;
    uint64_t peerVer = 0;
    if(hello.kind != Kind::HELLO) {
        failMsg(MsgErr::BAD_STATE, "bad_state");
        loopDone();
        return;
    }
    if(!hello.seq || !helloFromJson(hello.body, inst, peerEpoch, peerVer)) {
        failMsg(MsgErr::BAD_FIELD, "bad_hello");
        loopDone();
        return;
    }

    uint64_t target = m_verFn();  // 锁外调，避免锁内持有外部锁
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_inst = std::move(inst);
        m_helloSeq = hello.seq;
        m_targetVer = target;
        if(peerEpoch == m_epoch && peerVer <= target) {
            m_sentVer = peerVer;
            m_sentOnce = true;
            m_sent.store(peerVer, std::memory_order_relaxed);
            m_acked.store(peerVer, std::memory_order_relaxed);
        }
        m_ready = true;
    }
    if(m_readyFn) m_readyFn(shared_from_this());
    m_waker.notify();

    m_sock->setRecvTimeout((int64_t)m_ackMs);
    while(state() < State::CLOSING) {
        Msg msg;
        err = {};
        ret = recvMsg(m_sock, msg, &err);
        if(ret == MsgRet::OK) {
            m_rx.fetch_add(1, std::memory_order_relaxed);
            onMsg(msg);
            continue;
        }
        if(err.io == IoErr::TIMEOUT && err.frame == FrameErr::NONE) {
            onTimeout();
            continue;
        }
        failProto(err);
        break;
    }
    loopDone();
}

void Session::failProto(const ProtoErr& err) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(m_protoErr.io == IoErr::NONE && m_protoErr.frame == FrameErr::NONE) m_protoErr = err;
        if(m_why == StopWhy::NONE) {
            if(err.frame != FrameErr::NONE) m_why = StopWhy::FRAME;
            else if(err.io == IoErr::CLOSED) m_why = StopWhy::PEER;
            else if(err.io == IoErr::CANCELED && state() >= State::DRAIN) m_why = StopWhy::LOCAL;
            else m_why = StopWhy::IO;
        }
        if(state() < State::CLOSING) setState(State::CLOSING);
    }
    closeNow();
}

void Session::failMsg(MsgErr err, const std::string& code) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(m_msgErr == MsgErr::NONE) m_msgErr = err;
        if(m_why == StopWhy::NONE) m_why = StopWhy::MSG;
        if(m_sendErr.empty()) m_sendErr = code;
        if(state() < State::CLOSING) setState(State::CLOSING);
    }
    m_waker.notify();
}

void Session::closeNow() {
    if(m_sock) m_sock->close();
    m_waker.notify();
}

void Session::loopDone() {
    if(m_loops.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    setState(State::CLOSED);
    closeNow();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cv.notify_all();
    }
    StopWhy why;
    ProtoErr protoErr;
    MsgErr msgErr;
    std::string inst;
    uint64_t pending = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        why = m_why;
        protoErr = m_protoErr;
        msgErr = m_msgErr;
        inst = m_inst;
        pending = m_sync.seq ? m_sync.seq : m_ping.seq;
    }
    std::ostringstream line;
    line << "ipban link closed id=" << m_id
         << " inst=" << inst
         << " state=" << stateName(state())
         << " why=" << whyName(why)
         << " io=" << (int)protoErr.io
         << " frame=" << (int)protoErr.frame
         << " msg=" << (int)msgErr
         << " epoch=" << m_epoch
         << " sent=" << sentVersion()
         << " acked=" << ackedVersion()
         << " pending=" << pending
         << " alive_ms=" << aliveMs()
         << " rx=" << rxFrames()
         << " tx=" << txFrames();
    if(why == StopWhy::IO) BRONX_LOG_ERROR(g_log) << line.str();
    else if(why == StopWhy::FRAME || why == StopWhy::MSG
            || why == StopWhy::ACK_TIMEOUT || why == StopWhy::PONG_TIMEOUT)
        BRONX_LOG_WARN(g_log) << line.str();
    else BRONX_LOG_INFO(g_log) << line.str();
    if(m_doneFn) m_doneFn(shared_from_this());
}

void SessionRegistry::add(const Session::ptr& s) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sessions[s.get()] = s;
}

std::vector<Session::ptr> SessionRegistry::bind(const Session::ptr& s) {
    std::vector<Session::ptr> old;
    std::string inst = s->instanceId();
    for(const auto& peer : list()) {
        if(peer != s && !inst.empty() && peer->instanceId() == inst
           && peer->state() != Session::State::CLOSED) old.push_back(peer);
    }
    return old;
}

void SessionRegistry::remove(const Session::ptr& s) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sessions.erase(s.get());
}

std::vector<Session::ptr> SessionRegistry::list() {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<Session::ptr> out;
    out.reserve(m_sessions.size());
    for(auto& it : m_sessions) out.push_back(it.second);
    return out;
}

size_t SessionRegistry::size() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_sessions.size();
}

void SessionRegistry::stopAll() {
    for(auto& s : list()) s->beginStop(Session::StopWhy::LOCAL, true);
}

void SessionRegistry::forceAll() {
    for(auto& s : list()) s->forceStop();
}

} // namespace ipban
} // namespace bronx
