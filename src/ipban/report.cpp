#include "report.h"
#include "proto.h"
#include "wire.h"
#include "reactor.h"
#include "endpoint.h"
#include "metrics.h"
#include "util.h"
#include "log.h"
#include <algorithm>
#include <chrono>

namespace bronx {
namespace ipban {

static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("system");
static constexpr uint64_t kRiskCoolMs = 60000;

static size_t srcSlot(Src src) {
    size_t i = static_cast<size_t>(src);
    return i < 6 ? i : static_cast<size_t>(Src::RATE);
}

bool Reporter::tryReport(const Risk& r) {
    if(!m_accepting.load(std::memory_order_acquire)) {
        auto& metrics = gateway::GatewayMetrics::instance();
        metrics.incr_risk_stop_drop();
        metrics.note_risk(srcSlot(r.src), 3);
        return false;
    }
    size_t depth = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if(!m_accepting.load(std::memory_order_relaxed)) {
            auto& metrics = gateway::GatewayMetrics::instance();
            metrics.incr_risk_stop_drop();
            metrics.note_risk(srcSlot(r.src), 3);
            return false;
        }
        if(m_queue.size() >= m_opts.maxQueue) {
            auto& metrics = gateway::GatewayMetrics::instance();
            metrics.incr_risk_dropped();
            metrics.note_risk(srcSlot(r.src), 2);
            return false;
        }
        uint64_t now = bronx::GetCurrentMs();
        if(!r.id.empty()) {
            auto it = m_seen.find(r.id);
            if(it != m_seen.end() && now >= it->second && now - it->second < kRiskCoolMs) {
                gateway::GatewayMetrics::instance().note_risk(srcSlot(r.src), 1);
                return false;
            }
            if(m_seen.size() >= m_opts.maxQueue && !m_seen.empty()) m_seen.erase(m_seen.begin());
            m_seen[r.id] = now;
        }
        m_queue.push_back(r);
        depth = m_queue.size();
    }
    auto& metrics = gateway::GatewayMetrics::instance();
    metrics.set_risk_queue(depth);
    metrics.note_risk(srcSlot(r.src), 0);
    m_waker.notify();
    return true;
}

size_t Reporter::queueDepth() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_queue.size();
}

void Reporter::start(bronx::BxIoManager* iom) {
    bool expected = false;
    if(!iom || m_opts.submitPath.empty() || !m_opts.maxQueue || !m_opts.minBackoffMs
       || m_opts.maxBackoffMs < m_opts.minBackoffMs || !m_opts.stopTimeoutMs
       || !m_started.compare_exchange_strong(expected, true)) return;
    m_iom = iom;
    m_running.store(true, std::memory_order_release);
    auto self = shared_from_this();
    iom->post([self]() { self->loop(); });
    if(queueDepth()) m_waker.notify();
}

void Reporter::stop() {
    if(!m_started.load(std::memory_order_acquire)) {
        m_accepting.store(false, std::memory_order_release);
        dropRest();
        return;
    }
    bool was = m_accepting.exchange(false, std::memory_order_acq_rel);
    if(was) m_stopAt.store(bronx::GetCurrentMs() + m_opts.stopTimeoutMs,
                           std::memory_order_release);
    m_waker.notify();
    if(bronx::BxIoManager::Current() == m_iom) return;
    if(waitStop(m_opts.stopTimeoutMs + 100)) return;
    m_running.store(false, std::memory_order_release);
    bronx::BxSocket::ptr sock;
    {
        std::lock_guard<std::mutex> lk(m_sockMtx);
        sock = m_sock;
    }
    if(sock) sock->close();
    m_waker.notify();
    dropRest();
}

bool Reporter::waitStop(uint64_t ms) {
    std::unique_lock<std::mutex> lk(m_doneMtx);
    return m_doneCv.wait_for(lk, std::chrono::milliseconds(ms), [this]() {
        return m_done.load(std::memory_order_acquire);
    });
}

bool Reporter::stopDue() const {
    if(m_accepting.load(std::memory_order_acquire)) return false;
    uint64_t due = m_stopAt.load(std::memory_order_acquire);
    return !due || bronx::GetCurrentMs() >= due;
}

bool Reporter::waitBackoff(uint64_t ms) {
    if(stopDue()) return false;
    uint64_t due = m_stopAt.load(std::memory_order_acquire);
    if(due) {
        uint64_t now = bronx::GetCurrentMs();
        if(now >= due) return false;
        ms = std::min(ms, due - now);
    }
    std::weak_ptr<Reporter> weak = shared_from_this();
    auto timer = m_iom->addTimer(ms, [weak]() {
        if(auto reporter = weak.lock()) reporter->m_waker.notify();
    });
    {
        std::lock_guard<std::mutex> lk(m_timerMtx);
        m_backoffTimer = timer;
    }
    m_waker.wait();
    {
        std::lock_guard<std::mutex> lk(m_timerMtx);
        timer->cancel();
        if(m_backoffTimer == timer) m_backoffTimer.reset();
    }
    return m_running.load(std::memory_order_acquire) && !stopDue();
}

void Reporter::loop() {
    uint64_t backoff = m_opts.minBackoffMs;
    bronx::BxSocket::ptr sock;
    while(m_running.load(std::memory_order_acquire)) {
        if(queueDepth() == 0) {
            if(!m_accepting.load(std::memory_order_acquire)) break;
            if(!m_waker.wait()) break;
            continue;
        }
        if(stopDue()) break;
        if(!sock) {
            sock = bronx::BxSocket::MakeUnixTcpSocket();
            auto addr = bronx::BxUnixAddress::Create(m_opts.submitPath);
            {
                std::lock_guard<std::mutex> lk(m_sockMtx);
                m_sock = sock;
            }
            if(!sock || !addr || !sock->connect(addr, 2000)) {
                gateway::GatewayMetrics::instance().incr_risk_retry();
                if(sock) sock->close();
                {
                    std::lock_guard<std::mutex> lk(m_sockMtx);
                    if(m_sock == sock) m_sock.reset();  // connect 失败也要清，否则 stop() 看到悬挂 sock
                }
                sock.reset();
                if(!waitBackoff(backoff)) break;
                backoff = std::min(backoff * 2, m_opts.maxBackoffMs);
                continue;
            }
            sock->setSendTimeout(2000);
            backoff = m_opts.minBackoffMs;
        }
        if(!drainOnce(sock)) {
            sock->close();
            sock.reset();
            continue;
        }
    }
    if(sock) sock->close();
    {
        std::lock_guard<std::mutex> lk(m_sockMtx);
        m_sock.reset();
    }
    dropRest();
    finish();
}

bool Reporter::drainOnce(const bronx::BxSocket::ptr& sock) {
    while(m_running.load(std::memory_order_acquire)) {
        if(stopDue()) return true;
        Risk risk;
        size_t depth = 0;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if(m_queue.empty()) return true;
            risk = m_queue.front();
            m_queue.pop_front();
            depth = m_queue.size();
        }
        gateway::GatewayMetrics::instance().set_risk_queue(depth);
        std::string body = riskToJson(risk);
        if(body.size() > kMaxBody) {
            auto& metrics = gateway::GatewayMetrics::instance();
            metrics.incr_risk_dropped();
            metrics.note_risk(srcSlot(risk.src), 5);
            continue;
        }
        if(!sendMsg(sock, Kind::RISK, body)) {
            auto& metrics = gateway::GatewayMetrics::instance();
            metrics.note_risk(srcSlot(risk.src), 5);
            metrics.incr_risk_retry();
            std::lock_guard<std::mutex> lk(m_mtx);
            if(m_queue.size() >= m_opts.maxQueue && !m_queue.empty()) {
                metrics.note_risk(srcSlot(m_queue.back().src), 2);
                m_queue.pop_back();
                metrics.incr_risk_dropped();
            }
            m_queue.push_front(std::move(risk));
            gateway::GatewayMetrics::instance().set_risk_queue(m_queue.size());
            return false;
        }
        auto& metrics = gateway::GatewayMetrics::instance();
        metrics.incr_risk_sent();
        metrics.note_risk(srcSlot(risk.src), 4);
    }
    return false;
}

void Reporter::dropRest() {
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        dropped = m_queue.size();
        for(const auto& risk : m_queue)
            gateway::GatewayMetrics::instance().note_risk(srcSlot(risk.src), 3);
        m_queue.clear();
    }
    if(dropped) gateway::GatewayMetrics::instance().incr_risk_stop_drop(dropped);
    gateway::GatewayMetrics::instance().set_risk_queue(0);
}

void Reporter::finish() {
    m_running.store(false, std::memory_order_release);
    m_done.store(true, std::memory_order_release);
    m_doneCv.notify_all();
    BRONX_LOG_INFO(g_log) << "ipban reporter stopped";
}

} // namespace ipban
} // namespace bronx
