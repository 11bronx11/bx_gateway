#pragma once

// 代理终结，请求流式转到上游，响应流式回传，body 全程不整块进内存，
// 读一块写一块，SSE 也能转。连接从 UpstreamGroup 拿，带 LB 熔断连接池，
// 响应定界干净上游没 close 才回池复用，否则关掉。

#include "ctx.h"
#include "ip.h"
#include <vector>

namespace bronx {
namespace gateway {

// 转发结果
enum class ForwardResult {
    OK,
    CONNECT_FAIL,    // 连不上上游 → 502
    SEND_FAIL,       // 发请求失败 → 502
    UPSTREAM_TIMEOUT,// 上游超时 → 504
    BAD_RESPONSE,    // 上游响应不可解析 → 502
    BAD_REQUEST_BODY,// 客户端请求体畸形 → 400
    REQUEST_TOO_LARGE,// 客户端请求体过大 → 413
    EXPECT_FAIL,     // 不支持的 Expect → 417
    UPSTREAM_BUSY,
};

class Upstream {
public:
    // 执行一次转发:ctx 必须已 route.matched。
    // 上游连接超时/响应首字节超时(ms), trusted=可信代理名单(解析真实客户端 IP)。
    static ForwardResult forward(ReqCtx& ctx,
                                 uint64_t connectTimeoutMs = 3000,
                                 uint64_t recvTimeoutMs = 30000,
                                 const std::vector<bronx::ipban::Ip>& trusted = {});
};

} // namespace gateway
} // namespace bronx
