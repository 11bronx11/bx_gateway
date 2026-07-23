#include "ups_group.h"
#include "endpoint.h"
#include "net_socket.h"
#include "str.h"
#include "util.h"
#include <algorithm>
#include <cerrno>
#include <mutex>
#include <sstream>
#include <cstdlib>
#include <limits>
#include <unistd.h>

namespace bronx {
namespace gateway {

// WeightedRr
size_t WeightedRr::select(const std::vector<LbCand>& c) {
    if(c.empty()) return SIZE_MAX;
    std::lock_guard lk(m_mutex);
    bool changed = m_cur.size() != c.size()
                || m_indexes.size() != c.size()
                || m_weights.size() != c.size();
    if(!changed) {
        for(size_t i = 0; i < c.size(); ++i) {
            if(m_indexes[i] != c[i].index || m_weights[i] != c[i].weight) {
                changed = true;
                break;
            }
        }
    }
    if(changed) {
        m_cur.assign(c.size(), 0);
        m_indexes.resize(c.size());
        m_weights.resize(c.size());
        for(size_t i = 0; i < c.size(); ++i) {
            m_indexes[i] = c[i].index;
            m_weights[i] = c[i].weight;
        }
        m_zero_wt_cnt = 0;
    }
    int64_t total = 0;
    for(size_t i = 0; i < c.size(); ++i) {
        m_cur[i] += (int64_t)c[i].weight;
        total += (int64_t)c[i].weight;
    }
    if(total <= 0) {
        return c[m_zero_wt_cnt++ % c.size()].index;
    }
    size_t best = 0;
    for(size_t i = 1; i < c.size(); ++i)
        if(m_cur[i] > m_cur[best]) best = i;
    m_cur[best] -= total;
    return c[best].index;
}

bool IsSupportedLoadBalancer(const std::string& type) {
    return type == "round_robin" || type == "weighted" || type == "least_conn"
        || type == "weighted_least_conn";
}

LoadBalancer::ptr MakeLoadBalancer(const std::string& type) {
    if(type == "weighted") return std::make_shared<WeightedRr>();
    if(type == "least_conn") return std::make_shared<LeastConn>();
    if(type == "weighted_least_conn") return std::make_shared<WeightLeast>();
    return std::make_shared<RrLb>();
}

// Endpoint
Endpoint::Endpoint(const std::string& h, uint32_t p, uint32_t w,
                   const CircuitBreakerConfig& cbCfg, const ConnPoolConfig& poolCfg,
                   uint32_t max)
    : host(h), port(p), weight(w)
    , maxInflight(std::min<uint32_t>(max, (uint32_t)std::numeric_limits<int32_t>::max()))
    , cb(std::make_shared<CircuitBreaker>(cbCfg))
{
    auto addr = bronx::BxAddress::ResolveOneIp(h);
    if(addr) addr->setPort(p);
    pool = std::make_shared<ConnectionPool>(addr ? addr : bronx::BxAddress::ptr{}, poolCfg);
}

bool Endpoint::tryHold() {
    int32_t cur = activeConns.load(std::memory_order_relaxed);
    while(cur >= 0 && cur < std::numeric_limits<int32_t>::max()
            && (maxInflight == 0 || (uint32_t)cur < maxInflight)) {
        if(activeConns.compare_exchange_weak(cur, cur + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
    }
    return false;
}

void Endpoint::dropHold() {
    int32_t cur = activeConns.load(std::memory_order_relaxed);
    while(cur > 0 && !activeConns.compare_exchange_weak(cur, cur - 1,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {}
}

// UpstreamGroup
UpstreamGroup::UpstreamGroup(const std::string& name, LoadBalancer::ptr lb)
    : m_name(name), m_lb(std::move(lb)) {}

void UpstreamGroup::addEndpoint(Endpoint::ptr ep) {
    m_endpoints.push_back(std::move(ep));
}

AcqConn UpstreamGroup::tryAcquire(uint64_t connectTimeoutMs, uint64_t dueMs) {
    if(m_endpoints.empty()) {
        AcqConn out;
        out.why = AcqWhy::DOWN;
        return out;
    }

    std::vector<LbCand> candidates;
    AcqWhy last = AcqWhy::DOWN;
    for(size_t i = 0; i < m_endpoints.size(); ++i) {
        auto& ep = m_endpoints[i];
        if(!ep->healthy.load(std::memory_order_relaxed)) {
            ep->stat.reject(UpWhy::DOWN);
            continue;
        }
        if(!ep->cb->canTry()) {
            ep->stat.reject(UpWhy::OPEN);
            last = AcqWhy::OPEN;
            continue;
        }
        candidates.push_back({i, ep->weight, ep->activeConns.load()});
    }

    while(!candidates.empty()) {
        if(dueMs && leftMs(dueMs) == 0) {
            for(const auto& c : candidates) m_endpoints[c.index]->stat.reject(UpWhy::DEADLINE);
            AcqConn out;
            out.why = last == AcqWhy::TIMEOUT ? AcqWhy::TIMEOUT : AcqWhy::DEADLINE;
            return out;
        }
        size_t idx = m_lb->select(candidates);
        if(idx >= m_endpoints.size()) break;

        auto& ep = m_endpoints[idx];
        if(!ep->healthy.load(std::memory_order_relaxed)) {
            ep->stat.reject(UpWhy::DOWN);
            last = AcqWhy::DOWN;
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [idx](const LbCand& c) { return c.index == idx; }),
                candidates.end());
            continue;
        }
        if(!ep->tryHold()) {
            ep->stat.reject(UpWhy::BUSY);
            last = AcqWhy::BUSY;
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [idx](const LbCand& c) { return c.index == idx; }),
                candidates.end());
            continue;
        }
        uint64_t turn = 0;
        if(!ep->cb->isAllowed(&turn)) {
            ep->dropHold();
            ep->stat.reject(UpWhy::OPEN);
            last = AcqWhy::OPEN;
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [idx](const LbCand& c) { return c.index == idx; }),
                candidates.end());
            continue;
        }
        uint64_t wait = connectTimeoutMs;
        if(dueMs) wait = std::min(wait, leftMs(dueMs));
        if(wait == 0) {
            UpRet ret{UpMark::SKIP, UpWhy::DEADLINE};
            ep->cb->record(ret, turn);
            ep->dropHold();
            ep->stat.reject(UpWhy::DEADLINE);
            AcqConn out;
            out.why = AcqWhy::DEADLINE;
            return out;
        }
        // 半开态探针必须新建连接:池里可能积压了上游故障期间静默断掉的连接,
        // MSG_PEEK 探活抓不到无 FIN 的断连,用脏池连接会导致探针失败→熔断重开→振荡。
        bool halfOpen = (ep->cb->state() == CircuitBreaker::State::HALF_OPEN);
        bool fromPool = false;
        uint64_t begin = upMs();
        errno = 0;
        auto sock = halfOpen ? ep->pool->createFresh(wait)
                             : ep->pool->acquire(wait, &fromPool);
        if(!sock) {
            bool timeout = errno == ETIMEDOUT || (dueMs && leftMs(dueMs) == 0);
            UpRet ret{UpMark::FAIL, timeout ? UpWhy::TIMEOUT : UpWhy::CONNECT,
                      0, upMs() - begin};
            if(ret.costMs >= ep->cb->config().slowMs) ret.slow = true;
            ep->stat.add(ret);
            ep->cb->record(ret, turn);
            ep->dropHold();
            last = timeout ? AcqWhy::TIMEOUT : AcqWhy::CONNECT;
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [idx](const LbCand& c) { return c.index == idx; }),
                candidates.end());
            continue;
        }
        AcqConn out;
        out.endpoint = ep.get();
        out.sock = std::move(sock);
        out.reused = fromPool;
        out.turn = turn;
        out.held = true;
        return out;
    }
    AcqConn out;
    out.why = last;
    return out;
}

void UpstreamGroup::mark(AcqConn& conn, UpRet ret) {
    if(!conn.endpoint || conn.marked) return;
    if(ret.costMs >= conn.endpoint->cb->config().slowMs) ret.slow = true;
    conn.endpoint->stat.add(ret);
    conn.endpoint->cb->record(ret, conn.turn);
    conn.marked = true;
}

void UpstreamGroup::release(AcqConn& conn, const UpRet& ret, bool reusable) {
    if(!conn.endpoint) return;
    mark(conn, ret);
    conn.endpoint->pool->release(std::move(conn.sock), reusable);
    if(conn.held) conn.endpoint->dropHold();
    conn.endpoint = nullptr;
    conn.held = false;
}

void UpstreamGroup::release(AcqConn& conn, bool healthy, bool reusable) {
    UpRet ret;
    ret.mark = healthy ? UpMark::OK : UpMark::FAIL;
    ret.why = healthy ? UpWhy::NONE : UpWhy::CONNECT;
    release(conn, ret, reusable);
}

static bool sendAll(const bronx::BxSocket::ptr& sock, const std::string& data,
                    uint32_t timeoutMs) {
    uint64_t deadline = bronx::GetCurrentMs() + timeoutMs;
    size_t off = 0;
    while(off < data.size()) {
        int n = sock->send(data.data() + off, data.size() - off);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if(bronx::GetCurrentMs() >= deadline) {
                errno = ETIMEDOUT;
                return false;
            }
            usleep(1000);
            continue;
        }
        if(n <= 0) return false;
        off += n;
    }
    return true;
}

static int recv_some(const bronx::BxSocket::ptr& sock, char* buf, size_t len,
                    uint32_t timeoutMs) {
    uint64_t deadline = bronx::GetCurrentMs() + timeoutMs;
    while(true) {
        int n = sock->recv(buf, len);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if(bronx::GetCurrentMs() >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }
            usleep(1000);
            continue;
        }
        return n;
    }
}

static bool check_ep_http(const Endpoint::ptr& ep, const HealthCheckConfig& cfg) {
    auto addr = bronx::BxAddress::ResolveOneIp(ep->host);
    if(!addr) return false;
    addr->setPort(ep->port);
    auto sock = bronx::BxSocket::MakeTcp(addr);
    if(!sock || !sock->connect(addr, cfg.timeoutMs)) return false;
    sock->setRecvTimeout(cfg.timeoutMs);
    sock->setSendTimeout(cfg.timeoutMs);
    std::ostringstream req;
    req << "GET " << (cfg.path.empty() ? "/" : cfg.path) << " HTTP/1.1\r\n"
        << "Host: " << host_port(ep->host, ep->port) << "\r\n"
        << "Connection: close\r\n\r\n";
    if(!sendAll(sock, req.str(), cfg.timeoutMs)) return false;
    char buf[256];
    std::string head;
    while(head.find("\r\n") == std::string::npos && head.size() < 1024) {
        int n = recv_some(sock, buf, sizeof(buf), cfg.timeoutMs);
        if(n <= 0) return false;
        head.append(buf, n);
    }
    int status = 0;
    if(head.compare(0, 5, "HTTP/") == 0) {
        auto sp = head.find(' ');
        if(sp != std::string::npos && sp + 4 <= head.size()) {
            status = atoi(head.c_str() + sp + 1);
        }
    }
    ep->lastHealthStatus.store(status, std::memory_order_relaxed);
    return status >= 200 && status < 400;
}

void UpstreamGroup::runHealthCheckOnce() {
    if(!m_health.enabled) return;
    uint32_t healthyNeed = m_health.healthyThreshold ? m_health.healthyThreshold : 1;
    uint32_t unhealthyNeed = m_health.unhealthyThreshold ? m_health.unhealthyThreshold : 1;
    for(auto& ep : m_endpoints) {
        ep->healthChecks.fetch_add(1, std::memory_order_relaxed);
        bool ok = check_ep_http(ep, m_health);
        if(ok) {
            uint32_t s = ep->healthSuccesses.fetch_add(1, std::memory_order_relaxed) + 1;
            ep->healthFailures.store(0, std::memory_order_relaxed);
            if(s >= healthyNeed) {
                ep->healthy.store(true, std::memory_order_relaxed);
            }
        } else {
            ep->healthCheckFails.fetch_add(1, std::memory_order_relaxed);
            uint32_t f = ep->healthFailures.fetch_add(1, std::memory_order_relaxed) + 1;
            ep->healthSuccesses.store(0, std::memory_order_relaxed);
            if(f >= unhealthyNeed) {
                ep->healthy.store(false, std::memory_order_relaxed);
            }
        }
    }
}

} // namespace gateway
} // namespace bronx
