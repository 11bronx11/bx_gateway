#pragma once

// 从 YAML 文件加载配置，BuildSnapshot 一次建齐路由，上游注册表和中间件链。
// YAML 五段，server upstreams routes auth ip_filter，解析失败直接抛。

#include "gateway.h"
#include "middlewares/builtin.h"
#include "middlewares/stubs.h"
#include "jwt.h"
#include <memory>
#include <string>

namespace YAML { class Node; }

namespace bronx {
namespace ipban { class Guard; class Reporter; struct StaticPolicy; }
namespace gateway {

class GatewayConfig {
public:
    // 兼容旧接口：仅加载路由
    static Router::ptr LoadRouterFromFile(const std::string& path);
    static Router::ptr LoadRouterFromYaml(const YAML::Node& node);

    // Round 2：全量加载，构建 ConfigSnapshot（失败抛 std::runtime_error）
    // iom：装 health-check timer 用的 IoManager。传 nullptr 时退回 BxIoManager::Current()
    // ——这保证在 CpuPool 线程上 offload 构建时(Current()==nullptr)仍能装上 timer,
    // 调用方(reload)须在 reactor 协程上先抓 Current() 再传入。老调用点省略即旧行为。
    // guard：网关的 IP 名单账本。非空时把静态 ip_filter 规则灌进去并装名单中间件;
    // 为空(旧调用点/测试)则退回旧的静态 IPFilter 中间件, 行为不变。
    // reporter：坏 IP 上报口。非空时限流和 waf 命中会 tryReport 一条 risk 给 daemon;
    // 为空(旧调用点/测试)则只挡不报, 行为不变。
    static ConfigSnapshot::ptr BuildSnapshot(const std::string& path,
                                             bronx::BxIoManager* iom = nullptr,
                                             std::shared_ptr<bronx::ipban::Guard> guard = nullptr,
                                             std::shared_ptr<bronx::ipban::Reporter> reporter = nullptr);
    static ConfigSnapshot::ptr BuildSnapshotFromYaml(const YAML::Node& root,
                                                     bronx::BxIoManager* iom = nullptr,
                                                     std::shared_ptr<bronx::ipban::Guard> guard = nullptr,
                                                     std::shared_ptr<bronx::ipban::Reporter> reporter = nullptr);
    static ConfigSnapshot::ptr BuildSnapshotFromYaml(const YAML::Node& root,
                                                     bronx::BxIoManager* iom,
                                                     std::shared_ptr<bronx::ipban::Guard> guard,
                                                     std::shared_ptr<bronx::ipban::Reporter> reporter,
                                                     bronx::ipban::StaticPolicy* pending);
};

} // namespace gateway
} // namespace bronx
