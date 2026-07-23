// 网关只保留一条同步链
#pragma once

#include "guard.h"
#include "proto.h"
#include "waker.h"
#include "net_socket.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace bronx {
class BxIoManager;
class BxTimer;
namespace ipban {

struct SyncOpts {
    std::string subscribePath = "/tmp/bronx_ip_subscribe.sock";
    std::string instanceId = "gw-1";
    uint64_t minBackoffMs = 200;
    uint64_t maxBackoffMs = 5000;
    uint64_t recvTimeoutMs = 30000;
    uint64_t expiryTickMs = 1000;
    uint64_t stopTimeoutMs = 2000;
};

class SyncClient : public std::enable_shared_from_this<SyncClient> {
public:
    using ptr = std::shared_ptr<SyncClient>;

    enum class State {
        DOWN, DIAL, HELLO, SYNC, OPEN, BACKOFF, STOPPING, STOPPED
    };

    SyncClient(Guard::ptr guard, const SyncOpts& opts) : m_guard(std::move(guard)), m_opts(opts) {}

    void start(bronx::BxIoManager* iom);
    void stop();
    bool waitStop(uint64_t ms);
    State state() const { return m_state.load(std::memory_order_acquire); }

private:
    void loop();
    bool session(bool& fast);
    bool sendOne(const bronx::BxSocket::ptr& sock, Kind kind,
                 const std::string& body, uint64_t seq = 0);
    bool waitBackoff(uint64_t ms);
    void setState(State state);
    void updateAges();
    void finish();

    Guard::ptr m_guard;
    SyncOpts m_opts;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_started{false};
    std::atomic<State> m_state{State::DOWN};
    bronx::BxIoManager* m_iom = nullptr;

    std::mutex m_sockMtx;
    bronx::BxSocket::ptr m_sock;
    Waker m_wait;
    std::mutex m_timerMtx;
    std::shared_ptr<bronx::BxTimer> m_expiryTimer;
    std::shared_ptr<bronx::BxTimer> m_backoffTimer;

    std::mutex m_doneMtx;
    std::condition_variable m_doneCv;
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_stopForce{false};
    std::atomic<uint64_t> m_lastRx{0};
    std::atomic<uint64_t> m_lastAck{0};
    std::atomic<uint64_t> m_targetVer{0};
    uint64_t m_metricId = 0;
    uint64_t m_nextSeq = 1;
};

} // namespace ipban
} // namespace bronx
