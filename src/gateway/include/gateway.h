#pragma once

// 网关主服务，配置全装在 ConfigSnapshot 里，路由，上游表，中间件链一块。
// 每个请求开头拿一份快照引用用到底，reload 时整个换掉，在途请求还用旧的，新请求用新的。
// 热更新只走 admin 口的 POST /reload，SIGHUP 是留给别处的不碰这。

#include "tcp_listener.h"
#include "opts.h"
#include "http_msg.h"
#include "router.h"
#include "ups_group.h"
#include "mw.h"
#include "ip.h"
#include "timer.h"
#include <functional>
#include <mutex>
#include <vector>

namespace bronx {
namespace ipban { class Guard; class Reporter; class SyncClient; }
namespace gateway {

class GatewayConnection;

// 可替换的配置快照：路由表 + 上游注册 + 中间件链
struct ConfigSnapshot {
    using ptr = std::shared_ptr<ConfigSnapshot>;
    ~ConfigSnapshot() {
        for(auto& t : healthTimers) {
            if(t) t->cancel();
        }
    }
    Router::ptr           router;
    UpstreamRegistry::ptr upstreams;
    MwChain::ptr  chain;
    std::vector<bronx::ipban::Ip> trustedProxies;
    std::vector<bronx::BxTimer::ptr> healthTimers;
};

class GatewayServer : public bronx::BxTcpServer {
public:
    using ptr = std::shared_ptr<GatewayServer>;
    using RequestHandler = std::function<void(GatewayConnection&)>;

    GatewayServer(const GatewayOptions& opts = GatewayOptions(),
                  bronx::BxIoManager* ioworker    = bronx::BxIoManager::Current(),
                  bronx::BxIoManager* acceptWorker = bronx::BxIoManager::Current());
    ~GatewayServer() override;

    const GatewayOptions& options() const { return m_opts; }

    // ConfigSnapshot 管理
    // 设置初始快照（启动时调用）
    void setConfig(ConfigSnapshot::ptr cfg) {
        ConfigSnapshot::ptr old;
        {
            std::lock_guard lk(m_cfgMtx);
            old = std::move(m_cfg);
            m_cfg = std::move(cfg);
        }
        // 旧快照在锁外析构，cancel timer 不持 m_cfgMtx
    }
    // 取当前快照（每个请求调用，无锁拷贝 shared_ptr）
    ConfigSnapshot::ptr getConfig() const {
        std::lock_guard lk(m_cfgMtx);
        return m_cfg;
    }
    // 热更新：从文件重建 ConfigSnapshot 并 swap。失败保留旧快照，返回 false。
    bool reload(const std::string& cfgPath);

    // cfgPath 缓存（reload("") 使用当前路径）
    void setCfgPath(const std::string& p) {
        std::lock_guard lk(m_cfgMtx);
        m_cfgPath = p;
    }
    std::string cfgPath() const {
        std::lock_guard lk(m_cfgMtx);
        return m_cfgPath;
    }

    // 旧接口兼容（G2 桩）
    void setRequestHandler(RequestHandler h) { m_handler = std::move(h); }
    const RequestHandler& requestHandler() const { return m_handler; }

    // IP 名单账本。稳定持有, reload 不换; 中间件链每次重建都捕同一个它。
    // 首次访问惰性建好, 保证非空。
    std::shared_ptr<bronx::ipban::Guard> ipGuard();

    // 旧启动接口。
    void setReporter(std::shared_ptr<bronx::ipban::Reporter> r);
    std::shared_ptr<bronx::ipban::Reporter> reporter();

protected:
    void onConnection(bronx::BxSocket::ptr client) override;

private:
    GatewayOptions          m_opts;
    RequestHandler          m_handler;

    mutable std::mutex      m_cfgMtx;
    ConfigSnapshot::ptr     m_cfg;
    std::string             m_cfgPath;

    std::shared_ptr<bronx::ipban::Guard> m_guard;   // 惰性建, ipGuard() 保证非空
    std::shared_ptr<bronx::ipban::Reporter> m_reporter;
    std::shared_ptr<bronx::ipban::SyncClient> m_sync;
    std::string             m_submitPath;
    std::string             m_subscribePath;
    std::string             m_instanceId;
};

} // namespace gateway
} // namespace bronx
