#pragma once

#include "daemon.h"
#include "tcp_listener.h"

namespace bronx {
namespace ipban {

class HubHttp : public bronx::BxTcpServer {
public:
    using ptr = std::shared_ptr<HubHttp>;

    HubHttp(Daemon* daemon, bronx::BxIoManager* iom);

protected:
    void onConnection(bronx::BxSocket::ptr client) override;

private:
    Daemon* m_daemon;
};

} // namespace ipban
} // namespace bronx
