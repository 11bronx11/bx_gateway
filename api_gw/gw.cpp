// 网关进程。Bronx 负责 Env、框架配置和 CPU pool,业务只拉起 IOM+服务。
#include "gateway.h"
#include "conn.h"
#include "conf.h"
#include "admin.h"
#include "ctx.h"
#include "mw.h"
#include "reactor.h"
#include "endpoint.h"
#include "log.h"
#include "app.h"
#include "sync.h"
#include "anchor.h"
#include "crash.h"
#include <yaml-cpp/yaml.h>
#include <exception>

using namespace bronx::gateway;
static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("gw");

struct App : bronx::BxApplication {
    App() {
        // 先锚到 repo 根: 成功就把框架配置改成绝对路径(绕开 Env 的相对拼接),
        // 业务配置/日志/db 也随新 cwd 落到 root。失败(无 /proc)退回老约定: 从 repo 根跑。
        if(auto root = bronx::anchorRoot(); !root.empty()) {
            m_root = root;
            setConfigPath(root + "/api_gw/bin/bronx.yml");
        } else {
            setConfigPath("../api_gw/bin/bronx.yml");
        }
    }

    // 崩溃栈落地文件绝对路径(anchor 后 cwd 已是 repo 根, root 为空则退回相对)
    std::string crashFile() const {
        return (m_root.empty() ? std::string("logs") : m_root + "/logs") + "/crash.log";
    }

protected:
    bool onInit() override {
        try {
            auto root = YAML::LoadFile(m_cf);
            if(auto s = root["server"]) {
                if(s["address"])       m_biz = s["address"].as<std::string>();
                if(s["admin_address"]) m_adm = s["admin_address"].as<std::string>();
                if(s["io_workers"])    m_iomWorkers = s["io_workers"].as<size_t>();
                if(s["maintenance"])   m_maintenance = s["maintenance"].as<bool>();
            }
        } catch(const std::exception& e) {
            BRONX_LOG_ERROR(g_log) << "business config read: " << e.what();
            return false;
        }
        if(m_biz.empty() || m_iomWorkers == 0) {
            BRONX_LOG_ERROR(g_log) << "bad business process config";
            return false;
        }
        MaintGate::instance().set(m_maintenance);
        return true;
    }

    bool onStart() override {
        try {
            m_iom = std::make_unique<bronx::BxIoManager>(m_iomWorkers, "iom");
            m_adminIom = std::make_unique<bronx::BxIoManager>(1, "admin-iom");
        } catch(const std::exception& e) {
            BRONX_LOG_ERROR(g_log) << "iom start: " << e.what();
            return false;
        }
        bronx::BxSemaphore sem(0);
        bool ok = false;
        m_iom->post([this, &sem, &ok] { ok = up(); sem.notify(); });
        sem.wait();
        return ok;
    }

    void onStopBegin() override {
        if(m_admin) {
            m_admin->drain();
            if(!m_admin->waitDrain()) BRONX_LOG_WARN(g_log) << "admin drain timed out";
            m_admin->stop();
            m_admin.reset();
        }
        if(m_gw) {
            m_gw->drain();
            if(!m_gw->waitDrain()) BRONX_LOG_WARN(g_log) << "gateway drain timed out";
            m_gw->stop();
            m_gw.reset();
        }
    }

    void onStopEnd() override {
        if(m_iom) { m_iom->stop(); m_iom.reset(); }
        if(m_adminIom) { m_adminIom->stop(); m_adminIom.reset(); }
    }

    void onReload() override {
        BRONX_LOG_INFO(g_log) << "framework config reloaded";
    }

private:
    bool up() {
        m_gw = std::make_shared<GatewayServer>();
        if(!m_gw->reload(m_cf)) {
            BRONX_LOG_ERROR(g_log) << "reload failed"; return false;
        }

        // handler 只管跑当前快照的洋葱链, reload 换 chain 原子生效
        // 用 weak_ptr 捕获，停机后 lock() 返回 nullptr 直接返回，不访问悬空指针
        std::weak_ptr<GatewayServer> weak = m_gw;
        m_gw->setRequestHandler([weak](GatewayConnection& c){
            auto gw = weak.lock();
            if(!gw) return;
            auto cfg = gw->getConfig();
            ReqCtx ctx(&c);
            if(cfg && cfg->chain) {
                if(TraceGate::instance().on()) {
                    loadClientAddr(ctx, cfg->trustedProxies);
                    c.startTrace(ctx.clientAddr().ok ? ctx.clientAddr().client.toString()
                                                     : std::string());
                }
                cfg->chain->run(ctx);
            }
        });

        if(auto a = bronx::BxAddress::LookupAny(m_biz); !a || !m_gw->bind(a)) {
            BRONX_LOG_ERROR(g_log) << "bind " << m_biz << " failed"; return false;
        }
        m_admin = std::make_shared<AdminServer>(m_gw.get(), m_adminIom.get(), m_adminIom.get());
        if(auto a = bronx::BxAddress::LookupAny(m_adm); a && m_admin->bind(a))
            m_admin->start();
        else
            BRONX_LOG_WARN(g_log) << "admin bind failed, disabled";

        auto snap = m_gw->getConfig();
        BRONX_LOG_INFO(g_log) << "up biz=" << m_biz << " adm=" << m_adm
            << " routes=" << (snap && snap->router ? snap->router->routeCount() : 0u);
        m_gw->start();
        return true;
    }

    const std::string                   m_cf{"api_gw/bin/gateway.yml"};
    std::string                         m_biz{"0.0.0.0:8090"};
    std::string                         m_adm{"127.0.0.1:9090"};
    size_t                              m_iomWorkers{4};
    bool                                m_maintenance{false};
    std::unique_ptr<bronx::BxIoManager> m_iom;
    std::unique_ptr<bronx::BxIoManager> m_adminIom; // admin 专用，不抢业务 iom
    GatewayServer::ptr                  m_gw;
    AdminServer::ptr                    m_admin;
    std::string                         m_root;
};

int main(int argc, char** argv) {
    App app;
    if(!app.init(argc, argv)) return 1;
    // init 成功(日志/配置就绪)后装崩溃 handler, 崩溃栈打到 logs/crash.log
    bronx::installCrashHandler(app.crashFile().c_str());
    return app.run();
}
