#include "ronduan.h"
#include <algorithm>

namespace bronx {
namespace gateway {

CircuitBreaker::CircuitBreaker(const CircuitBreakerConfig& cfg)
    : m_cfg(cfg) {
    m_cfg.failureThreshold = std::max(1u, m_cfg.failureThreshold);
    m_cfg.windowMs = std::max(1u, m_cfg.windowMs);
    m_cfg.buckets = std::max(1u, m_cfg.buckets);
    m_cfg.minRequests = std::max(1u, m_cfg.minRequests);
    m_cfg.failureRate = std::min(100u, m_cfg.failureRate);
    m_cfg.slowMs = std::max(1u, m_cfg.slowMs);
    m_cfg.slowRate = std::min(100u, m_cfg.slowRate);
    m_cfg.openTimeoutMs = std::max(1u, m_cfg.openTimeoutMs);
    m_cfg.maxOpenTimeoutMs = std::max(m_cfg.openTimeoutMs, m_cfg.maxOpenTimeoutMs);
    m_cfg.halfOpenMaxRequests = std::max(1u, m_cfg.halfOpenMaxRequests);
    m_cfg.halfOpenSuccesses = std::max(1u, m_cfg.halfOpenSuccesses);
    m_cfg.halfOpenSuccesses = std::min(m_cfg.halfOpenSuccesses, m_cfg.halfOpenMaxRequests);
    m_open_ms = m_cfg.openTimeoutMs;
    m_buckets.resize(m_cfg.buckets);
}

CircuitBreaker::State CircuitBreaker::state() const {
    std::lock_guard lk(m_mutex);
    return m_state;
}

bool CircuitBreaker::isAllowed(uint64_t* turn) {
    std::lock_guard lk(m_mutex);
    if(m_state == State::CLOSED) {
        if(turn) *turn = m_turn;
        return true;
    }
    if(m_state == State::OPEN) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_openAt).count();
        if((uint64_t)elapsed >= m_open_ms) {
            toHalfOpen();
        } else {
            return false;
        }
    }
    if(m_half_open_pending < m_cfg.halfOpenMaxRequests) {
        ++m_half_open_pending;
        if(turn) *turn = m_turn;
        return true;
    }
    return false;
}

bool CircuitBreaker::canTry() const {
    std::lock_guard lk(m_mutex);
    if(m_state == State::CLOSED) {
        return true;
    }
    if(m_state == State::HALF_OPEN) {
        return m_half_open_pending < m_cfg.halfOpenMaxRequests;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_openAt).count();
    return m_cfg.halfOpenMaxRequests > 0
        && (uint64_t)elapsed >= m_open_ms;
}

void CircuitBreaker::record(const UpRet& ret, uint64_t turn) {
    std::lock_guard lk(m_mutex);
    if(turn != 0 && turn != m_turn) return;
    if(m_state == State::OPEN) return;

    if(m_state == State::HALF_OPEN) {
        if(m_half_open_pending > 0) --m_half_open_pending;
        if(ret.mark == UpMark::SKIP) return;
        if(ret.mark == UpMark::FAIL) {
            toOpen(true);
            return;
        }
        if(++m_half_open_ok >= m_cfg.halfOpenSuccesses) toClosed();
        return;
    }

    if(ret.mark == UpMark::SKIP) return;
    if(ret.mark == UpMark::FAIL) ++m_consec_fail;
    else m_consec_fail = 0;
    uint64_t now = upMs();
    addBucket(ret, now);
    CbSnap s = snapLocked(now);
    bool rateOpen = s.requests >= m_cfg.minRequests
        && ((m_cfg.failureRate > 0 && s.failureRate >= m_cfg.failureRate)
            || (m_cfg.slowRate > 0 && s.slowRate >= m_cfg.slowRate));
    if(m_consec_fail >= m_cfg.failureThreshold || rateOpen) toOpen(false);
}

void CircuitBreaker::recordSuccess() {
    UpRet ret;
    ret.mark = UpMark::OK;
    record(ret);
}

void CircuitBreaker::recordFailure() {
    UpRet ret;
    ret.mark = UpMark::FAIL;
    ret.why = UpWhy::CONNECT;
    record(ret);
}

void CircuitBreaker::toOpen(bool again) {
    if(again) {
        uint64_t next = (uint64_t)m_open_ms * 2;
        m_open_ms = (uint32_t)std::min<uint64_t>(next, m_cfg.maxOpenTimeoutMs);
    } else {
        m_open_ms = m_cfg.openTimeoutMs;
    }
    move(State::OPEN);
    m_openAt = std::chrono::steady_clock::now();
    m_half_open_pending = 0;
    m_half_open_ok = 0;
}

void CircuitBreaker::toClosed() {
    move(State::CLOSED);
    m_consec_fail = 0;
    m_half_open_pending = 0;
    m_half_open_ok = 0;
    m_open_ms = m_cfg.openTimeoutMs;
    for(auto& b : m_buckets) b = {};
}

void CircuitBreaker::toHalfOpen() {
    move(State::HALF_OPEN);
    m_half_open_pending = 0;
    m_half_open_ok = 0;
}

void CircuitBreaker::move(State to) {
    if(m_state == to) return;
    size_t from = (size_t)m_state;
    size_t next = (size_t)to;
    ++m_moves[from * 3 + next];
    m_state = to;
    ++m_turn;
}

void CircuitBreaker::addBucket(const UpRet& ret, uint64_t now) {
    uint64_t width = std::max<uint64_t>(1, m_cfg.windowMs / m_cfg.buckets);
    uint64_t tick = now / width;
    CbBucket& b = m_buckets[tick % m_buckets.size()];
    if(b.tick != tick) b = CbBucket{tick, 0, 0, 0};
    ++b.requests;
    if(ret.mark == UpMark::FAIL) ++b.failures;
    if(ret.slow) ++b.slows;
}

CbSnap CircuitBreaker::snapLocked(uint64_t now) const {
    CbSnap out;
    uint64_t width = std::max<uint64_t>(1, m_cfg.windowMs / m_cfg.buckets);
    uint64_t tick = now / width;
    uint64_t keep = (m_cfg.windowMs + width - 1) / width;
    for(const auto& b : m_buckets) {
        if(b.tick == 0 || tick < b.tick || tick - b.tick >= keep) continue;
        out.requests += b.requests;
        out.failures += b.failures;
        out.slows += b.slows;
    }
    if(out.requests) {
        out.failureRate = (uint32_t)(out.failures * 100 / out.requests);
        out.slowRate = (uint32_t)(out.slows * 100 / out.requests);
    }
    out.openMs = m_open_ms;
    out.pending = m_half_open_pending;
    out.halfOpenOk = m_half_open_ok;
    out.moves = m_moves;
    return out;
}

CbSnap CircuitBreaker::snapshot() const {
    std::lock_guard lk(m_mutex);
    return snapLocked(upMs());
}

bool CircuitBreaker::badStatus(int status) const {
    return std::find(m_cfg.failureStatuses.begin(), m_cfg.failureStatuses.end(), status)
        != m_cfg.failureStatuses.end();
}

} // namespace gateway
} // namespace bronx
