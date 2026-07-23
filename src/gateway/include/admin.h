#pragma once

// 运维接口，绑在独立的 admin 口上，跟业务口分开。
// healthz 探活，stats 出 JSON 指标，routes 看路由表，reload POST 触发热重载。

#include "gateway.h"
#include <string>

namespace bronx {
namespace gateway {

// 独立 Admin 服务（绑定 admin_address，复用 BxIoManager）
class AdminServer : public bronx::BxTcpServer {
public:
    using ptr = std::shared_ptr<AdminServer>;

    AdminServer(GatewayServer* gw,
                bronx::BxIoManager* ioworker    = bronx::BxIoManager::Current(),
                bronx::BxIoManager* acceptWorker = bronx::BxIoManager::Current());

protected:
    void onConnection(bronx::BxSocket::ptr client) override;

private:
    GatewayServer* m_gw;
};

} // namespace gateway
} // namespace bronx
