#include "test_util.h"
#include "gateway.h"
#include "guard.h"
#include "ip.h"
#include "reactor.h"
#include "metrics.h"
#include <atomic>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace bronx;
using namespace bronx::gateway;

static void writeCfg(const std::string& path, const std::string& submit,
                     const std::string& subscribe, const std::string& blocked = "1.2.3.4",
                     bool badAuth = false) {
    std::ofstream out(path, std::ios::trunc);
    out << "upstreams:\n"
        << "  - name: backend\n"
        << "    lb: round_robin\n"
        << "    endpoints:\n"
        << "      - host: 127.0.0.1\n"
        << "        port: 9\n"
        << "routes:\n"
        << "  - name: all\n"
        << "    path: /\n"
        << "    upstream: backend\n"
        << "ip_filter:\n"
        << "  enabled: true\n"
        << "  mode: denylist\n"
        << "  cidrs: [" << blocked << "]\n"
        << "ip_policy:\n"
        << "  submit_sock: " << submit << "\n"
        << "  subscribe_sock: " << subscribe << "\n"
        << "  instance_id: reload-test\n";
    if(badAuth) {
        out << "auth:\n"
            << "  jwt:\n"
            << "    enabled: true\n"
            << "    leeway_sec: nope\n";
    }
}

static bronx::ipban::Ip ip(const char* s) {
    bronx::ipban::Ip out;
    bronx::ipban::parseCidr(s, out);
    return out;
}

static bool wait(std::atomic<bool>& done) {
    for(int i = 0; i < 500 && !done.load(); ++i) usleep(10 * 1000);
    return done.load();
}

int main() {
    std::string base = "/tmp/bronx_ipban_reload_" + std::to_string(::getpid());
    std::string cfg = base + ".yml";
    std::string submit1 = base + "_submit1.sock";
    std::string sub1 = base + "_sub1.sock";
    std::string submit2 = base + "_submit2.sock";
    std::string sub2 = base + "_sub2.sock";
    writeCfg(cfg, submit1, sub1);

    BxIoManager iom(2, "ipban-reload");
    GatewayServer gw(GatewayOptions(), &iom, &iom);
    gw.setCfgPath(cfg);
    std::atomic<bool> firstDone{false};
    bool firstOk = false;
    iom.post([&]() {
        firstOk = gw.reload("");
        firstDone.store(true);
    });
    TEST_CHECK(wait(firstDone));
    TEST_CHECK(firstOk);
    auto firstReporter = gw.reporter();
    TEST_CHECK(firstReporter != nullptr);

    writeCfg(cfg, submit2, sub2);
    std::atomic<bool> secondDone{false};
    bool secondOk = false;
    iom.post([&]() {
        secondOk = gw.reload("");
        secondDone.store(true);
    });
    TEST_CHECK(wait(secondDone));
    TEST_CHECK(secondOk);
    TEST_CHECK(gw.reporter() != nullptr);
    TEST_CHECK(gw.reporter() != firstReporter);
    usleep(100 * 1000);
    TEST_CHECK(GatewayMetrics::instance().snapshot().ipbanLinkState != 7);

    writeCfg(cfg, submit2, sub2, "2.2.2.2", true);
    std::atomic<bool> badDone{false};
    bool badOk = true;
    iom.post([&]() {
        badOk = gw.reload("");
        badDone.store(true);
    });
    TEST_CHECK(wait(badDone));
    TEST_CHECK(!badOk);
    auto guard = gw.ipGuard();
    TEST_CHECK(guard->eval(ip("1.2.3.4")).action == bronx::ipban::Act::DENY);
    TEST_CHECK(guard->eval(ip("2.2.2.2")).action == bronx::ipban::Act::ALLOW);

    ::unlink(cfg.c_str());
    return TEST_SUMMARY();
}
