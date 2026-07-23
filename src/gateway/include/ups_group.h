#pragma once

// 一组上游 endpoint 的编排，把负载均衡，熔断，连接池串起来。
// tryAcquire 是 LB 选一个，问熔断器放不放，再从池里取连接，release 反过来归还加记账。
// UpstreamRegistry 就是 name 到 UpstreamGroup 的全局表，配置加载时填。

#include "ronduan.h"
#include "conn_pool.h"
#include "lb.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>

namespace bronx {
namespace gateway {

struct HealthCheckConfig {
    bool        enabled = false;
    std::string path = "/healthz";
    uint32_t    intervalMs = 5000;
    uint32_t    timeoutMs = 1000;
    uint32_t    healthyThreshold = 1;
    uint32_t    unhealthyThreshold = 2;
};

struct Endpoint {
    using ptr = std::shared_ptr<Endpoint>;

    std::string            host;
    uint32_t               port   = 80;
    uint32_t               weight = 1;

    CircuitBreaker::ptr    cb;
    ConnectionPool::ptr    pool;
    std::atomic<int32_t>   activeConns{0};
    uint32_t               maxInflight = 0;
    UpStat                 stat;
    std::atomic<bool>      healthy{true};
    std::atomic<uint32_t>  healthSuccesses{0};
    std::atomic<uint32_t>  healthFailures{0};
    std::atomic<uint64_t>  healthChecks{0};
    std::atomic<uint64_t>  healthCheckFails{0};
    std::atomic<int>       lastHealthStatus{0};

    Endpoint(const std::string& h, uint32_t p, uint32_t w,
             const CircuitBreakerConfig& cbCfg, const ConnPoolConfig& poolCfg,
             uint32_t max = 0);

    bool tryHold();
    void dropHold();
};

// 一次成功 acquire 的结果；调用方持有期间 activeConns 已 +1
struct AcqConn {
    Endpoint*          endpoint = nullptr; // 裸指针，生命周期与 UpstreamGroup 绑定
    bronx::BxSocket::ptr sock;
    bool               reused = false;
    AcqWhy             why = AcqWhy::NONE;
    uint64_t           turn = 0;
    bool               marked = false;
    bool               held = false;
};

class UpstreamGroup {
public:
    using ptr = std::shared_ptr<UpstreamGroup>;

    explicit UpstreamGroup(const std::string& name, LoadBalancer::ptr lb);

    void addEndpoint(Endpoint::ptr ep);
    void setHealthCheck(const HealthCheckConfig& cfg) { m_health = cfg; }
    const HealthCheckConfig& healthCheck() const { return m_health; }
    void setTimeouts(uint64_t total, uint64_t connect, uint64_t read) {
        m_total_ms = total;
        m_connect_ms = connect;
        m_read_ms = read;
    }
    uint64_t totalMs() const { return m_total_ms; }
    uint64_t connectMs() const { return m_connect_ms; }
    uint64_t readMs() const { return m_read_ms; }

    // 选 endpoint + 取连接。全部 OPEN 或连接失败 → AcqConn{nullptr, nullptr}。
    AcqConn tryAcquire(uint64_t connectTimeoutMs = 3000, uint64_t dueMs = 0);

    // 归还连接并记录结果（healthy=true → CB.recordSuccess，false → CB.recordFailure）
    // 归还连接：healthy 记录熔断器（请求成败），reusable 决定是否入池复用。
    // reusable=true 需调用方确保：keep-alive + 响应精确定界 + 缓冲已排空 + 无错误。
    void mark(AcqConn& conn, UpRet ret);
    void release(AcqConn& conn, const UpRet& ret, bool reusable = false);
    void release(AcqConn& conn, bool healthy, bool reusable = false);

    const std::string& name() const { return m_name; }
    const char* lbName() const { return m_lb ? m_lb->name() : "unknown"; }
    size_t endpointCount() const { return m_endpoints.size(); }
    std::vector<Endpoint::ptr> endpoints() const { return m_endpoints; }
    void runHealthCheckOnce();

private:
    std::string                 m_name;
    std::vector<Endpoint::ptr>  m_endpoints;
    LoadBalancer::ptr           m_lb;
    HealthCheckConfig           m_health;
    uint64_t                    m_total_ms = 0;
    uint64_t                    m_connect_ms = 0;
    uint64_t                    m_read_ms = 0;
};

// 全局上游注册表
class UpstreamRegistry {
public:
    using ptr = std::shared_ptr<UpstreamRegistry>;

    void add(UpstreamGroup::ptr g) { m_map[g->name()] = std::move(g); }
    UpstreamGroup::ptr get(const std::string& name) const {
        auto it = m_map.find(name);
        return it != m_map.end() ? it->second : nullptr;
    }
    const std::unordered_map<std::string, UpstreamGroup::ptr>& all() const { return m_map; }

private:
    std::unordered_map<std::string, UpstreamGroup::ptr> m_map;
};

} // namespace gateway
} // namespace bronx
