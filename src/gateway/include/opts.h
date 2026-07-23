#pragma once

// 网关的运行参数，超时和准入限制都堆这。
// GatewayServer 持有，连接协程读它决定啥时候超时啥时候拒。

#include <cstdint>
#include <cstddef>

namespace bronx {
namespace gateway {

struct GatewayOptions {
    // -- 超时(毫秒)--
    uint64_t headerTimeoutMs = 10 * 1000;   // 读完请求头的超时
    uint64_t bodyTimeoutMs   = 30 * 1000;   // 读完请求体的超时
    uint64_t idleTimeoutMs   = 60 * 1000;   // keep-alive 空闲超时(等下个请求)

    // -- 准入限制 --
    size_t   maxHeaderSize   = 8 * 1024;     // 请求头块上限(超 → 431)
    uint64_t maxBodySize     = 16ull * 1024 * 1024;  // 请求体上限(超 → 413)
    uint32_t maxKeepAliveRequests = 1000;    // 单连接 keep-alive 最多处理请求数

    // -- 行为 --
    bool     keepAlive = true;               // 是否允许 keep-alive
};

} // namespace gateway
} // namespace bronx
