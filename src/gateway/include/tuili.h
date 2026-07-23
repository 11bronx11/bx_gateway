#pragma once

// 推理网关的三个扩展点，这轮只留接口和空实现。
// TokenEstimator 让限流按 token 数扣而不是按请求数，默认返回 1。
// ModelKeyExtractor 让路由按 body 里的 model 字段走而不是 path。
// StreamFilter 挂在流式响应的 chunk 回调上，数 output token 或注水印，默认啥也不做。

#include "ctx.h"
#include <string>
#include <functional>
#include <memory>

namespace bronx {
namespace gateway {

// TokenEstimator：估算本次请求消耗的 token 数，用于 RateLimit 扣减量
class TokenEstimator {
public:
    using ptr = std::shared_ptr<TokenEstimator>;
    virtual ~TokenEstimator() = default;
    // 默认按请求数（返回 1）；接入时实现 tokenizer 估算
    virtual size_t estimate(ReqCtx& /*ctx*/) { return 1; }
};

// ModelKeyExtractor：从请求上下文提取 model key，用于按模型路由
// 接入：router.setKeyExtractor(ModelKeyExtractor)
// 默认行为（接入前）：router 使用 path 作为 key，与此接口无关。
using ModelKeyExtractor = std::function<std::string(ReqCtx&)>;

// StreamFilter：流式响应 chunk 级回调
class StreamFilter {
public:
    using ptr = std::shared_ptr<StreamFilter>;
    virtual ~StreamFilter() = default;
    // 每个响应 chunk 调用一次；chunk 是原始字节（SSE frame 等）
    virtual void onChunk(std::string_view /*chunk*/, ReqCtx& /*ctx*/) {}
    // 响应流结束时调用
    virtual void onEnd(ReqCtx& /*ctx*/) {}
};

} // namespace gateway
} // namespace bronx
