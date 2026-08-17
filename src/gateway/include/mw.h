#pragma once

// 洋葱模型的中间件链。每个中间件三选一，调 next 放行，填响应短路，或 next 前后各做事。
// 按注册顺序执行，短路后已进入的中间件回程代码照跑，末端通常是代理转发。

#include "ctx.h"
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace bronx {
namespace gateway {

// next:调它就进链上的下一个中间件
using NextFn = std::function<void()>;

bool loadClientAddr(ReqCtx& ctx, const std::vector<bronx::ipban::Ip>& trusted = {});
void applyHeaders(ReqCtx& ctx, const GwResponse::ptr& rsp);
bool reply(ReqCtx& ctx, int status, const std::string& body,
           const HeaderMap& headers = {}, bool close = false);
bool reply(ReqCtx& ctx, int status);
void writeFail(ReqCtx& ctx);

// 中间件:函数式接口(也可包装有状态对象)
// 入参:ctx=请求上下文;next=继续链。中间件决定是否调 next。
class Middleware {
public:
    using ptr = std::shared_ptr<Middleware>;
    virtual ~Middleware() = default;
    virtual void handle(ReqCtx& ctx, const NextFn& next) = 0;
    virtual const char* name() const { return "middleware"; }
};

// 函数式中间件(lambda 包装,便于写内置中间件)
class FuncMiddleware : public Middleware {
public:
    using Fn = std::function<void(ReqCtx&, const NextFn&)>;
    FuncMiddleware(Fn fn, const char* name) : m_fn(std::move(fn)), m_name(name) {}
    void handle(ReqCtx& ctx, const NextFn& next) override { m_fn(ctx, next); }
    const char* name() const override { return m_name; }
private:
    Fn m_fn;
    const char* m_name;
};

// 洋葱链执行器
class MwChain {
public:
    using ptr = std::shared_ptr<MwChain>;

    // 注册中间件(按注册顺序为洋葱外→内)
    void use(Middleware::ptr m) { m_mws.push_back(std::move(m)); }
    void use(FuncMiddleware::Fn fn, const char* name) {
        m_mws.push_back(std::make_shared<FuncMiddleware>(std::move(fn), name));
    }

    // 从头跑一遍链,中途没人短路就走到链尾。连接的 handler 里调。
    void run(ReqCtx& ctx) const;

    size_t size() const { return m_mws.size(); }

private:
    // trace 开着时走的插桩版本,每个中间件打进入/离开/短路/自身耗时。
    // 单独一份是为了让常规 run 保持零额外分支。
    void run_traced(ReqCtx& ctx) const;

    std::vector<Middleware::ptr> m_mws;
};

} // namespace gateway
} // namespace bronx
