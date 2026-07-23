// 一个连接 一个读协程 一个写协程
#pragma once

#include "waker.h"
#include "proto.h"
#include "net_socket.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bronx {
class BxIoManager;
namespace ipban {

using PushFn = std::function<bool(uint64_t sentVer, bool sentOnce,
                                  Kind& kindOut, std::string& bodyOut, uint64_t& newVerOut)>;
using VerFn = std::function<uint64_t()>;

struct SyncWait {
    uint64_t seq = 0;
    uint64_t ver = 0;
    uint64_t due = 0;
};

struct PingWait {
    uint64_t seq = 0;
    uint64_t due = 0;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    using ptr = std::shared_ptr<Session>;

    enum class State {
        NEW, HELLO, SYNC, OPEN, DRAIN, CLOSING, CLOSED
    };

    enum class StopWhy {
        NONE, LOCAL, PEER, IO, FRAME, MSG, ACK_TIMEOUT, PONG_TIMEOUT,
        REPLACED, STOP_TIMEOUT
    };

    using ReadyFn = std::function<void(const ptr&)>;
    using DoneFn = std::function<void(const ptr&)>;

    Session(bronx::BxSocket::ptr sock, uint64_t epoch, VerFn verFn, PushFn pushFn,
            ReadyFn readyFn, DoneFn doneFn, uint64_t helloMs, uint64_t ackMs);

    void start(bronx::BxIoManager* iom);
    void wake();
    void beginStop(StopWhy why, bool drain);
    bool waitStop(uint64_t ms);
    void forceStop();

    State state() const { return m_state.load(std::memory_order_acquire); }
    StopWhy stopWhy() const;
    bool forced() const;
    uint64_t ackedVersion() const { return m_acked.load(std::memory_order_relaxed); }
    uint64_t sentVersion() const { return m_sent.load(std::memory_order_acquire); }
    std::string instanceId() const;
    bool running() const { return state() != State::CLOSED; }
    FrameErr frameErr() const;
    MsgErr msgErr() const;
    IoErr ioErr() const;
    uint64_t rxFrames() const { return m_rx.load(std::memory_order_relaxed); }
    uint64_t txFrames() const { return m_tx.load(std::memory_order_relaxed); }
    uint64_t aliveMs() const;

private:
    void recvLoop();
    void sendLoop();
    bool sendReady();
    bool sendCurrent();
    bool sendPing();
    bool sendOne(Kind kind, const std::string& body, uint64_t seq);
    void onMsg(const Msg& msg);
    void onTimeout();
    void failProto(const ProtoErr& err);
    void failMsg(MsgErr err, const std::string& code);
    void closeNow();
    void loopDone();
    void setState(State state);

    bronx::BxSocket::ptr m_sock;
    uint64_t m_epoch = 0;
    VerFn m_verFn;
    PushFn m_pushFn;
    ReadyFn m_readyFn;
    DoneFn m_doneFn;
    uint64_t m_helloMs = 0;
    uint64_t m_ackMs = 0;
    Waker m_waker;
    uint64_t m_id = 0;

    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::string m_inst;
    StopWhy m_why = StopWhy::NONE;
    ProtoErr m_protoErr;
    MsgErr m_msgErr = MsgErr::NONE;
    SyncWait m_sync;
    PingWait m_ping;
    uint64_t m_helloSeq = 0;
    uint64_t m_targetVer = 0;
    uint64_t m_sentVer = 0;
    uint64_t m_nextSeq = 1;
    std::string m_sendErr;
    bool m_ready = false;
    bool m_sentOnce = false;
    bool m_pushDue = false;
    bool m_dirty = false;
    bool m_forceFull = false;
    bool m_pingDue = false;
    bool m_forced = false;

    std::atomic<State> m_state{State::NEW};
    std::atomic<uint64_t> m_acked{0};
    std::atomic<uint64_t> m_sent{0};
    std::atomic<uint64_t> m_rx{0};
    std::atomic<uint64_t> m_tx{0};
    std::atomic<unsigned> m_loops{0};
    uint64_t m_startedAt = 0;
};

class SessionRegistry {
public:
    void add(const Session::ptr& s);
    std::vector<Session::ptr> bind(const Session::ptr& s);
    void remove(const Session::ptr& s);
    std::vector<Session::ptr> list();
    size_t size();
    void stopAll();
    void forceAll();

private:
    std::mutex m_mtx;
    std::unordered_map<Session*, Session::ptr> m_sessions;
};

class Broadcaster {
public:
    explicit Broadcaster(SessionRegistry* reg) : m_reg(reg) {}
    void publish(uint64_t) {
        for(auto& s : m_reg->list()) s->wake();
    }

private:
    SessionRegistry* m_reg;
};

} // namespace ipban
} // namespace bronx
