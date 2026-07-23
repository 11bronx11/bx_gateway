#pragma once

// 轻量 waf 中间件, 全局挂在洋葱链上, 只扫 url 和几个 header, 不碰 body。
// 一组编译好的正则找 sqli xss 路径穿越 扫描器指纹, 命中就 403 短路 + 关连接,
// 配了 reporter 再报一条坏 IP 给 daemon(src=WAF)。正则内联跑, 检测面小不 offload。

#include "mw.h"
#include "ip.h"
#include <memory>
#include <vector>

namespace bronx {
namespace ipban {

class Reporter;

// waf 开关和封禁建议。off 就整个中间件跳过, 行为跟没装一样。
struct WafCfg {
    bool     on    = false;
    uint64_t banMs = 600000;   // 命中建议封 10 分钟
};

// 造一个 waf 中间件。reporter 可空(只挡不报)。trustedProxies 拿来解析真实客户端 IP,
// 和名单 限流用同一套(空则只信 peer)。
bronx::gateway::Middleware::ptr MakeWaf(WafCfg cfg,
                                        std::shared_ptr<Reporter> reporter = nullptr,
                                        std::vector<Ip> trustedProxies = {});

} // namespace ipban
} // namespace bronx
