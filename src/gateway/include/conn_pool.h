#pragma once

// 到同一个上游 endpoint 的 TCP 连接池，省建连开销。
// acquire 优先复用空闲连接池空就新建，release 归还，超时或超上限就关掉。
// 归还后这条连接调用方别再碰。

#include "net_socket.h"
#include "endpoint.h"
#include <mutex>
#include <deque>
#include <memory>
#include <cstdint>
#include <chrono>

namespace bronx {
namespace gateway {

struct ConnPoolConfig {
    uint32_t maxIdle        = 32;    // 池中最大空闲连接数
    uint32_t idleTimeoutMs  = 30000; // 空闲超时(ms)，超时连接不入池
};

class ConnectionPool {
public:
    using ptr = std::shared_ptr<ConnectionPool>;

    ConnectionPool(bronx::BxAddress::ptr addr, const ConnPoolConfig& cfg = {});

    // 取连接：优先复用空闲连接，无可用则新建。失败返回 nullptr。
    // connectTimeoutMs：新建连接的超时。fromPool：非空时回填本次是否来自池（复用）。
    bronx::BxSocket::ptr acquire(uint64_t connectTimeoutMs = 3000, bool* fromPool = nullptr);

    // 强制新建一条连接（不从池取），用于复用连接失效后的重试。失败返回 nullptr。
    bronx::BxSocket::ptr createFresh(uint64_t connectTimeoutMs = 3000);

    // 归还连接：reusable=true 且复用前检查通过才尝试入池；false 或超 max_idle 则关闭。
    void release(bronx::BxSocket::ptr sock, bool reusable = true);

    size_t idleCount() const;
    const ConnPoolConfig& config() const { return m_cfg; }

private:
    struct Entry {
        bronx::BxSocket::ptr sock;
        std::chrono::steady_clock::time_point idleSince;
    };

    bronx::BxAddress::ptr m_addr;
    ConnPoolConfig      m_cfg;
    mutable std::mutex  m_mutex;
    std::deque<Entry>   m_idle;
};

} // namespace gateway
} // namespace bronx
