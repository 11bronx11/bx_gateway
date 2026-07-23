#include "gateway.h"
#include "conn.h"
#include "conf.h"
#include "log.h"
#include "reactor.h"
#include "offload.h"
#include "guard.h"
#include "pull.h"
#include "report.h"
#include <yaml-cpp/yaml.h>

namespace bronx {
namespace gateway {

static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

namespace {

struct IpCfg {
    std::string submit;
    std::string subscribe;
    std::string instance = "gw-1";
};

struct Loaded {
    ConfigSnapshot::ptr cfg;
    IpCfg ip;
    bronx::ipban::StaticPolicy policy;
    std::shared_ptr<bronx::ipban::Reporter> reporter;
};

static IpCfg readIpCfg(const YAML::Node& root) {
    IpCfg out;
    if(!root["ip_policy"]) return out;
    const auto& p = root["ip_policy"];
    if(p["submit_sock"]) out.submit = p["submit_sock"].as<std::string>();
    if(p["subscribe_sock"]) out.subscribe = p["subscribe_sock"].as<std::string>();
    if(p["instance_id"]) out.instance = p["instance_id"].as<std::string>();
    return out;
}

} // namespace

std::shared_ptr<bronx::ipban::Guard> GatewayServer::ipGuard() {
    std::lock_guard lk(m_cfgMtx);
    if(!m_guard) m_guard = std::make_shared<bronx::ipban::Guard>();
    return m_guard;
}

void GatewayServer::setReporter(std::shared_ptr<bronx::ipban::Reporter> r) {
    std::lock_guard lk(m_cfgMtx);
    m_reporter = std::move(r);
}

std::shared_ptr<bronx::ipban::Reporter> GatewayServer::reporter() {
    std::lock_guard lk(m_cfgMtx);
    return m_reporter;
}

GatewayServer::GatewayServer(const GatewayOptions& opts,
                             bronx::BxIoManager* ioworker,
                             bronx::BxIoManager* acceptWorker)
    : bronx::BxTcpServer(ioworker, acceptWorker)
    , m_opts(opts) {
    type_ = "gateway";
    setRecvTimeout(m_opts.headerTimeoutMs);
}

GatewayServer::~GatewayServer() {
    std::shared_ptr<bronx::ipban::Reporter> reporter;
    std::shared_ptr<bronx::ipban::SyncClient> sync;
    {
        std::lock_guard lk(m_cfgMtx);
        reporter = std::move(m_reporter);
        sync = std::move(m_sync);
    }
    if(reporter) reporter->stop();
    if(sync) sync->stop();
}

void GatewayServer::onConnection(bronx::BxSocket::ptr client) {
    GatewayConnection conn(client, this);
    conn.process();
}

bool GatewayServer::reload(const std::string& cfgPath) {
    std::string path;
    std::string oldSubmit;
    std::shared_ptr<bronx::ipban::Reporter> oldReporter;
    {
        std::lock_guard lk(m_cfgMtx);
        path = cfgPath.empty() ? m_cfgPath : cfgPath;
        oldSubmit = m_submitPath;
        oldReporter = m_reporter;
    }
    try {
        // reload 会做阻塞活:YAML::LoadFile(文件 IO) + ResolveOneIp(getaddrinfo,未 hook,
        // 真阻塞线程)。若就地在 reactor worker 协程里跑,会钉死该 worker(main 仅 iom(4),
        // 业务+admin 共享),reload-under-load 抖动。故 offload 到 CpuPool 跑,当前协程 eventfd
        // 等结果、期间让出——阻塞离开 reactor。
        // iom 必须在本 reactor 协程上抓取后传入:CpuPool 线程上 BxIoManager::Current()==nullptr,
        // 不传则新快照漏装 health-check timer。无 fiber/pool 上下文时 offload 退回 inline(启动/测试)。
        bronx::BxIoManager* iom = bronx::BxIoManager::Current();
        auto guard = ipGuard();
        auto loaded = bronx::offload([path, iom, guard, oldSubmit, oldReporter]() {
            YAML::Node root = YAML::LoadFile(path);
            Loaded out;
            out.ip = readIpCfg(root);
            if(!out.ip.submit.empty() && out.ip.submit == oldSubmit && oldReporter) {
                out.reporter = oldReporter;
            } else if(!out.ip.submit.empty()) {
                bronx::ipban::ReportOpts opts;
                opts.submitPath = out.ip.submit;
                out.reporter = std::make_shared<bronx::ipban::Reporter>(opts);
            }
            out.cfg = GatewayConfig::BuildSnapshotFromYaml(
                root, iom, guard, out.reporter, &out.policy);
            return out;
        });
        if(!loaded.cfg) {
            BRONX_LOG_ERROR(g_logger) << "reload failed: BuildSnapshot returned null, path=" << path;
            return false;
        }

        bool swapReporter = false;
        bool swapSync = false;
        std::shared_ptr<bronx::ipban::SyncClient> nextSync;
        std::shared_ptr<bronx::ipban::Reporter> retiredReporter;
        std::shared_ptr<bronx::ipban::SyncClient> retiredSync;
        {
            std::lock_guard lk(m_cfgMtx);
            if(cfgPath.empty() && path != m_cfgPath) {
                BRONX_LOG_WARN(g_logger) << "reload ignored stale path=" << path
                                         << ", current path=" << m_cfgPath;
                return false;
            }
            swapReporter = loaded.reporter != m_reporter;
            bool hasSync = !loaded.ip.subscribe.empty() || !m_subscribePath.empty();
            swapSync = hasSync && (loaded.ip.subscribe != m_subscribePath
                || loaded.ip.instance != m_instanceId
                || (!loaded.ip.subscribe.empty() && !m_sync));
            if((swapReporter || swapSync) && !iom) {
                BRONX_LOG_ERROR(g_logger) << "reload ip_policy needs an io manager";
                return false;
            }
            retiredReporter = swapReporter ? m_reporter : nullptr;
            retiredSync = swapSync ? m_sync : nullptr;
            if(!loaded.ip.subscribe.empty()) {
                if(swapSync) {
                    bronx::ipban::SyncOpts opts;
                    opts.subscribePath = loaded.ip.subscribe;
                    opts.instanceId = loaded.ip.instance;
                    nextSync = std::make_shared<bronx::ipban::SyncClient>(m_guard, opts);
                } else {
                    nextSync = m_sync;
                }
            }
            m_guard->applyStatic(std::move(loaded.policy));
            m_cfg = loaded.cfg;
            m_reporter = loaded.reporter;
            m_sync = nextSync;
            m_submitPath = loaded.ip.submit;
            m_subscribePath = loaded.ip.subscribe;
            m_instanceId = loaded.ip.instance;
            if(!cfgPath.empty()) m_cfgPath = cfgPath;
        }
        if(swapReporter && loaded.reporter) loaded.reporter->start(iom);
        if(swapSync && nextSync) nextSync->start(iom);
        if(retiredReporter) retiredReporter->stop();
        if(retiredSync) retiredSync->stop();
        BRONX_LOG_INFO(g_logger) << "gateway config reloaded from " << path;
        return true;
    } catch(const std::exception& e) {
        BRONX_LOG_ERROR(g_logger) << "reload exception: " << e.what();
        return false;
    }
}

} // namespace gateway
} // namespace bronx
