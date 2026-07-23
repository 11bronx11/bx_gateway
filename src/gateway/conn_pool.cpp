#include "conn_pool.h"
#include "io_hook.h"
#include <cerrno>
#include <sys/socket.h>

namespace bronx {
namespace gateway {

static bool sock_reusable(const bronx::BxSocket::ptr& sock) {
    if(!sock || !sock->isConnected()) {
        return false;
    }
    char c;
    while(true) {
        ssize_t n = recv_f(sock->getSocket(), &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if(n > 0) {
            return false;       // 有残留字节,复用会串到下一响应。
        }
        if(n == 0) {
            return false;       // 对端已经 FIN。
        }
        if(errno == EINTR) {
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;        // 暂无可读字节,连接仍可尝试复用。
        }
        return false;
    }
}

ConnectionPool::ConnectionPool(bronx::BxAddress::ptr addr, const ConnPoolConfig& cfg)
    : m_addr(std::move(addr)), m_cfg(cfg) {}

bronx::BxSocket::ptr ConnectionPool::acquire(uint64_t connectTimeoutMs, bool* fromPool) {
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lk(m_mutex);
        while(!m_idle.empty()) {
            auto& e = m_idle.front();
            auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - e.idleSince).count();
            if((uint32_t)idleMs >= m_cfg.idleTimeoutMs) {
                m_idle.pop_front();
                continue;
            }
            auto sock = std::move(e.sock);
            m_idle.pop_front();
            if(!sock_reusable(sock)) {
                continue;
            }
            if(fromPool) *fromPool = true;
            return sock;
        }
    }
    // 新建连接
    if(fromPool) *fromPool = false;
    if(!m_addr) {
        return nullptr;
    }
    auto sock = bronx::BxSocket::MakeTcp(m_addr);
    if(!sock) {
        return nullptr;
    }
    if(!sock->connect(m_addr, connectTimeoutMs)) {
        return nullptr;
    }
    return sock;
}

bronx::BxSocket::ptr ConnectionPool::createFresh(uint64_t connectTimeoutMs) {
    if(!m_addr) {
        return nullptr;
    }
    auto sock = bronx::BxSocket::MakeTcp(m_addr);
    if(!sock) {
        return nullptr;
    }
    if(!sock->connect(m_addr, connectTimeoutMs)) {
        return nullptr;
    }
    return sock;
}

void ConnectionPool::release(bronx::BxSocket::ptr sock, bool reusable) {
    if(!sock || !reusable) return;  // 不可复用直接丢弃（BxSocket 析构时关闭）
    if(!sock_reusable(sock)) {
        return;
    }
    std::lock_guard lk(m_mutex);
    if(m_idle.size() >= m_cfg.maxIdle) {
        return; // 超 max_idle，丢弃
    }
    m_idle.push_back({std::move(sock), std::chrono::steady_clock::now()});
}

size_t ConnectionPool::idleCount() const {
    std::lock_guard lk(m_mutex);
    return m_idle.size();
}

} // namespace gateway
} // namespace bronx
