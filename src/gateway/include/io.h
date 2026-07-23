#pragma once

// 完整写出一段字节，统一处理 EINTR EAGAIN 和短写。
// 网关里所有直接写 socket 的地方都走这个，省得各写各的边界语义对不上。

#include "net_socket.h"
#include "util.h"
#include <cstddef>
#include <cerrno>
#include <unistd.h>

namespace bronx {
namespace gateway {

// 写满 len 字节，返回已写字节数，失败返回 -1，errno 留底层错误。
// 在 worker 里 send 经 hook 让出协程，普通线程按原生阻塞跑。
inline int SendAll(const bronx::BxSocket::ptr& sock, const char* data, size_t len) {
    if(!sock || (!data && len > 0)) {
        errno = EINVAL;
        return -1;
    }
    int64_t timeout = sock->getSendTimeout();
    if(timeout < 0) {
        timeout = 60000;
    }
    uint64_t deadline = bronx::GetCurrentMs() + (uint64_t)timeout;
    size_t sent = 0;
    while(sent < len) {
        int n = sock->send(data + sent, len - sent);
        if(n < 0 && errno == EINTR) {
            continue;
        }
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // hook 开启时 send 内部已经等待过事件；这里兜底非 hook 非阻塞 fd。
            if(bronx::GetCurrentMs() >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }
            usleep(1000);
            continue;
        }
        if(n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

} // namespace gateway
} // namespace bronx
