#pragma once

// 路由缓存接口，缓存匹配结果省得重复查表，默认 NullRouteCache 不缓存。
// 想接 ARC 之类的，router.setCache 一行注进去就行。
// key 里带了 Router 的 generation，每次 reload 加一，旧 key 自然就 miss 掉了。

#include "ctx.h"
#include <string>
#include <cstdint>

namespace bronx {
namespace gateway {

class RouteCache {
public:
    using ptr = std::shared_ptr<RouteCache>;
    virtual ~RouteCache() = default;

    // 命中返回 true 并填 out；未命中返回 false
    virtual bool get(std::string_view key, RouteResult& out) = 0;
    virtual void put(std::string_view key, const RouteResult& result) = 0;
    // generation 变化时无需显式 invalidate（key 含 gen，旧 key 自然 miss）；
    // 提供此接口供需要主动清理的实现使用。
    virtual void invalidate() {}
    virtual void stats(uint64_t& hits, uint64_t& misses) const { hits = misses = 0; }
};

// 默认空实现：永远 miss，接口兼容，零开销
class NullRouteCache : public RouteCache {
public:
    bool get(std::string_view, RouteResult&) override { return false; }
    void put(std::string_view, const RouteResult&) override {}
};

} // namespace gateway
} // namespace bronx
