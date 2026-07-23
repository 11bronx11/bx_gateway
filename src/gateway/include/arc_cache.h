#pragma once

// ArcRouteCache: 用 ARC 算法实现 RouteCache 接口。
// 接入一行: router.setCache(make_shared<ArcRouteCache>(1024))
// key 格式: "<gen>|<method>|<path>"，reload 时 gen++ 旧 key 自然 miss，
// invalidate() 保留供主动清空。

#include "rt_cache.h"
#include "../../../tools/arc.h"
#include <string>

namespace bronx {
namespace gateway {

class ArcRouteCache : public RouteCache {
public:
    explicit ArcRouteCache(size_t cap = 1024)
        : m_arc(cap) {}

    bool get(std::string_view key, RouteResult& out) override {
        return m_arc.get(std::string(key), out);
    }

    void put(std::string_view key, const RouteResult& result) override {
        m_arc.put(std::string(key), result);
    }

    void invalidate() override {
        m_arc.invalidate();
    }

    void stats(uint64_t& hits, uint64_t& misses) const override {
        m_arc.stats(hits, misses);
    }

private:
    bronx::Arc<std::string, RouteResult> m_arc;
};

} // namespace gateway
} // namespace bronx
