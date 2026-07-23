#pragma once

#include "up_ret.h"
#include <array>
#include <mutex>
#include <cstdint>
#include <chrono>
#include <memory>
#include <vector>

namespace bronx {
namespace gateway {

struct CircuitBreakerConfig {
    uint32_t failureThreshold = 5;
    uint32_t windowMs = 10000;
    uint32_t buckets = 10;
    uint32_t minRequests = 20;
    uint32_t failureRate = 50;
    uint32_t slowMs = 1000;
    uint32_t slowRate = 50;
    uint32_t openTimeoutMs = 10000;
    uint32_t maxOpenTimeoutMs = 60000;
    uint32_t halfOpenMaxRequests = 1;
    uint32_t halfOpenSuccesses = 1;
    std::vector<int> failureStatuses{500, 502, 503, 504};
};

struct CbBucket {
    uint64_t tick = 0;
    uint64_t requests = 0;
    uint64_t failures = 0;
    uint64_t slows = 0;
};

struct CbSnap {
    uint64_t requests = 0;
    uint64_t failures = 0;
    uint64_t slows = 0;
    uint32_t failureRate = 0;
    uint32_t slowRate = 0;
    uint32_t openMs = 0;
    uint32_t pending = 0;
    uint32_t halfOpenOk = 0;
    std::array<uint64_t, 9> moves{};
};

class CircuitBreaker {
public:
    using ptr = std::shared_ptr<CircuitBreaker>;

    explicit CircuitBreaker(const CircuitBreakerConfig& cfg = {});

    bool isAllowed(uint64_t* turn = nullptr);
    bool canTry() const;
    void record(const UpRet& ret, uint64_t turn = 0);
    void recordSuccess();
    void recordFailure();

    // 当前状态（调试/metrics 用）
    enum class State { CLOSED, OPEN, HALF_OPEN };
    State state() const;
    CbSnap snapshot() const;
    bool badStatus(int status) const;
    const CircuitBreakerConfig& config() const { return m_cfg; }

private:
    void toOpen(bool again);
    void toClosed();
    void toHalfOpen();
    void move(State to);
    void addBucket(const UpRet& ret, uint64_t now);
    CbSnap snapLocked(uint64_t now) const;

    mutable std::mutex              m_mutex;
    CircuitBreakerConfig            m_cfg;
    State                           m_state = State::CLOSED;
    uint32_t                        m_consec_fail = 0;
    uint32_t                        m_half_open_pending = 0;
    uint32_t                        m_half_open_ok = 0;
    uint32_t                        m_open_ms = 0;
    uint64_t                        m_turn = 1;
    std::vector<CbBucket>           m_buckets;
    std::array<uint64_t, 9>         m_moves{};
    std::chrono::steady_clock::time_point m_openAt;
};

} // namespace gateway
} // namespace bronx
