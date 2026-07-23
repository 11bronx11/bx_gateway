#include "middlewares/stubs.h"

namespace bronx {
namespace gateway {

// 鉴权中间件:auth 为空视为放行;失败 → 401 短路。
Middleware::ptr MakeAuthMiddleware(Authenticator::ptr auth) {
    return std::make_shared<FuncMiddleware>(
        [auth](ReqCtx& ctx, const NextFn& next) {
            if(auth && auth->authenticate(ctx) != AuthErr::Ok) {
                reply(ctx, 401, "Unauthorized\n");
                return;
            }
            next();
        }, "auth");
}

} // namespace gateway
} // namespace bronx
