// banhub — 封禁控制面。三个 UDS 口: submit 收举报, subscribe 网关订阅, admin 管理。
// 给了 db 路径就 SQLite 落盘, 重启规则不丢; 不给全内存重启即丢。
// Bronx 负责 Env、框架配置和 CPU pool,业务只拉起 IOM+daemon+http。
#include "daemon.h"
#include "hub_http.h"
#include "reactor.h"
#include "endpoint.h"
#include "log.h"
#include "app.h"
#include "sync.h"
#include "anchor.h"
#include "crash.h"
#include <yaml-cpp/yaml.h>
#include <exception>

using namespace bronx::ipban;
static bronx::BxLogger::ptr g_log = BRONX_LOG_NAME("hub");

struct HubApp : bronx::BxApplication {
    HubApp() {
        // 先锚到 repo 根: 成功就把框架配置改成绝对路径(绕开 Env 的相对拼接),
        // 业务配置/日志/db 也随新 cwd 落到 root。失败(无 /proc)退回老约定: 从 repo 根跑。
        if(auto root = bronx::anchorRoot(); !root.empty()) {
            m_root = root;
            setConfigPath(root + "/api_gw/bin/hub_bronx.yml");
        } else {
            setConfigPath("../api_gw/bin/hub_bronx.yml");
        }
    }

    // crash.log 绝对路径, 与 gw 落同一目录
    std::string crashFile() const {
        return (m_root.empty() ? std::string("logs") : m_root + "/logs") + "/crash.log";
    }

protected:
    bool onInit() override {
        m_opts.submitPath    = "/tmp/bronx_ip_submit.sock";
        m_opts.subscribePath = "/tmp/bronx_ip_subscribe.sock";
        m_opts.adminPath     = "/tmp/bronx_ip_admin.sock";
        try {
            auto root = YAML::LoadFile(m_cf);
            if(auto s = root["server"]) {
                if(s["submit_sock"])    m_opts.submitPath    = s["submit_sock"].as<std::string>();
                if(s["subscribe_sock"]) m_opts.subscribePath = s["subscribe_sock"].as<std::string>();
                if(s["admin_sock"])     m_opts.adminPath     = s["admin_sock"].as<std::string>();
                if(s["http_address"])   m_httpAddr           = s["http_address"].as<std::string>();
                if(s["iom_workers"])    m_iomWorkers         = s["iom_workers"].as<size_t>();
                if(s["db_path"])        m_opts.dbPath        = s["db_path"].as<std::string>();
            }
        } catch(const std::exception& e) {
            BRONX_LOG_ERROR(g_log) << "business config read: " << e.what();
            return false;
        }
        if(m_opts.submitPath.empty() || m_opts.subscribePath.empty() || m_iomWorkers == 0) {
            BRONX_LOG_ERROR(g_log) << "bad business process config";
            return false;
        }
        return true;
    }

    bool onStart() override {
        try {
            m_iom = std::make_unique<bronx::BxIoManager>(m_iomWorkers, "hub");
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
        if(m_http) {
            m_http->drain();
            if(!m_http->waitDrain()) BRONX_LOG_WARN(g_log) << "hub http drain timed out";
            m_http->stop();
            m_http.reset();
        }
        if(m_daemon) { m_daemon->stop(); m_daemon.reset(); }
    }

    void onStopEnd() override {
        if(m_iom)    m_iom->stop();
        m_iom.reset();
    }

    void onReload() override {
        BRONX_LOG_INFO(g_log) << "framework config reloaded";
    }

private:
    bool up() {
        m_daemon = std::make_unique<Daemon>(m_iom.get(), m_opts);
        if(!m_daemon->start()) {
            BRONX_LOG_ERROR(g_log) << "daemon start failed";
            m_daemon.reset(); return false;
        }

        m_http = std::make_shared<HubHttp>(m_daemon.get(), m_iom.get());
        auto a = bronx::BxAddress::LookupAny(m_httpAddr);
        if(!a || !m_http->bind(a) || !m_http->start()) {
            BRONX_LOG_ERROR(g_log) << "hub http failed addr=" << m_httpAddr;
            m_http->stop();
            m_http.reset();
            m_daemon->beginStop();
            return false;
        }

        BRONX_LOG_INFO(g_log) << "hub up http=" << m_httpAddr
            << " db=" << (m_opts.dbPath.empty() ? "none" : m_opts.dbPath);
        return true;
    }

    std::string                              m_root;
    const std::string                        m_cf{"api_gw/bin/hub.yml"};
    size_t                                   m_iomWorkers{2};
    std::string                              m_httpAddr{"127.0.0.1:9091"};
    DaemonOpts                               m_opts;
    std::unique_ptr<bronx::BxIoManager>      m_iom;
    std::unique_ptr<Daemon>                  m_daemon;
    std::shared_ptr<HubHttp>                 m_http;
};

int main(int argc, char** argv) {
    HubApp app;
    if(!app.init(argc, argv)) return 1;
    bronx::installCrashHandler(app.crashFile().c_str());
    return app.run();
}
