#include "test_util.h"
#include "test_support.h"

#include <atomic>
#include <iostream>
#include <thread>

using namespace api_gw_test;

namespace {

bool replaceOnce(std::string& text, const std::string& from, const std::string& to) {
    const size_t pos = text.find(from);
    if(pos == std::string::npos) return false;
    text.replace(pos, from.size(), to);
    return true;
}

bool hasRoute(const Apps& apps, const std::string& name, const std::string& upstream) {
    auto response = requestOnce(apps.gatewayAdminPort(), "GET", "/routes");
    if(!response || response->status != 200) return false;
    Json::Value routes;
    if(!parseJson(*response, routes) || !routes["routes"].isArray()) return false;
    for(const auto& route : routes["routes"]) {
        if(route["name"].asString() == name && route["upstream"].asString() == upstream) return true;
    }
    return false;
}

std::string journeyFramework(const fs::path& logFile) {
    return
        "cpu_pool:\n"
        "  threads: 1\n"
        "  max_queue: 32\n"
        "  name: journey\n"
        "fiber:\n"
        "  stack_size: 131072\n"
        "  stack_pool_size: 2\n"
        "  guard_page: 1\n"
        "tcp:\n"
        "  connect:\n"
        "    timeout: 3000\n"
        "tcp_server:\n"
        "  recv_timeout: 10000\n"
        "logs:\n"
        "  - name: root\n"
        "    level: warn\n"
        "    appenders:\n"
        "      - type: BxStdoutLogAppender\n"
        "        async: false\n"
        "  - name: system\n"
        "    level: info\n"
        "    appenders:\n"
        "      - type: BxFileLogAppender\n"
        "        file: " + logFile.string() + "\n"
        "        async: true\n";
}

uintmax_t fileSize(const fs::path& path) {
    std::error_code error;
    const uintmax_t size = fs::file_size(path, error);
    return error ? 0 : size;
}

bool toolJson(const ToolResult& result, Json::Value& value) {
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in(result.output);
    return Json::parseFromStream(builder, in, &value, &errors);
}

} // namespace

int main() {
    bool ok = true;
    auto check = [&ok](bool value, const std::string& what) {
        TEST_CHECK_MSG(value, what);
        ok = ok && value;
    };

    Apps apps("journey", true);
    GatewaySpec spec;
    spec.ipFilterEnabled = true;
    spec.ipCidrs = {"198.51.100.40"};
    spec.rateLimit = true;
    spec.rateCapacity = 1;
    spec.rateRefillPerSec = 4;

    const fs::path logFile = apps.root() / "journey-system.log";
    const fs::path gwFramework = apps.root() / "gw-bronx.yml";

    // A1/A2, isolated config starts the full process set
    check(apps.prepare("journey", spec), "write isolated journey configs");
    check(writeText(gwFramework, journeyFramework(logFile)), "write cpu pool and async log config");
    const std::string gatewayYaml = readFile(apps.gatewayConfigPath());
    const std::string hubYaml = readFile(apps.root() / "api_gw/bin/hub.yml");
    check(gatewayYaml.find("address: 127.0.0.1:" + std::to_string(apps.businessPort()))
              != std::string::npos
          && gatewayYaml.find("admin_address: 127.0.0.1:" + std::to_string(apps.gatewayAdminPort()))
              != std::string::npos
          && hubYaml.find("http_address: 127.0.0.1:" + std::to_string(apps.hubHttpPort()))
              != std::string::npos,
          "business admin and hub ports come from isolated YAML");
    check(readFile(gwFramework).find("cpu_pool:\n  threads: 1") != std::string::npos,
          "framework config carries cpu pool settings");
    check(apps.startHub(), "start hub process");
    check(apps.waitHubReady(), "hub healthz and sockets are ready");
    check(apps.startGateway(), "start gateway process with -c framework config");
    check(apps.waitGatewayReady(), "gateway business listener is ready");
    check(threadCount(apps.gateway().pid) > 0 && threadCount(apps.hub().pid) > 0,
          "configured processes own running threads");

    // A3/A4, async logs and IOM stay live under concurrent keep-alive traffic
    const uintmax_t logStart = fileSize(logFile);
    constexpr int kClients = 4;
    constexpr int kRequestsPerClient = 3;
    std::atomic<int> burstOk{0};
    std::vector<std::thread> clients;
    for(int client = 0; client < kClients; ++client) {
        clients.emplace_back([&apps, &burstOk, client] {
            HttpClient http;
            if(!http.connect(apps.businessPort())) return;
            for(int request = 0; request < kRequestsPerClient; ++request) {
                HttpResponse response;
                const std::string ip = "198.51.101."
                    + std::to_string(client * kRequestsPerClient + request + 1);
                const bool keepAlive = request + 1 < kRequestsPerClient;
                if(http.request("GET", "/api/burst", "", {{"X-Forwarded-For", ip}}, keepAlive,
                                response)
                   && response.status == 200 && response.body == "mock:/burst:") {
                    ++burstOk;
                }
            }
        });
    }
    for(auto& client : clients) client.join();
    check(burstOk == kClients * kRequestsPerClient && alive(apps.gateway()),
          "concurrent keep-alive requests finish while gateway stays alive");
    check(waitUntil([&] { return fileSize(logFile) > logStart; }, 3000),
          "async system log appender writes request traffic to disk");

    // B1, health is served on both data and admin listeners
    auto businessHealth = requestOnce(apps.businessPort(), "GET", "/healthz");
    auto adminHealth = requestOnce(apps.gatewayAdminPort(), "GET", "/healthz");
    check(businessHealth && businessHealth->status == 200, "business healthz returns 200");
    check(adminHealth && adminHealth->status == 200, "admin healthz returns 200");

    // B2, static policy uses the trusted proxy client address
    auto staticDenied = apps.gatewayRequest("GET", "/api/static", "", "198.51.100.40");
    auto staticAllowed = apps.gatewayRequest("GET", "/api/static", "", "198.51.100.41");
    check(staticDenied && staticDenied->status == 403, "static CIDR deny reaches Guard");
    check(staticAllowed && staticAllowed->status == 200, "other client address reaches proxy");

    // B3, banctl changes cross hub subscribe and the gateway Guard
    const std::string bannedIp = "203.0.113.55";
    const ToolResult deny = runTool({
        Apps::appBinary("banctl").string(), "--sock", apps.adminSock().string(), "--json",
        "deny", bannedIp, "perm", "journey"
    }, apps.root());
    Json::Value denyJson;
    check(deny.exitCode == 0 && toolJson(deny, denyJson) && denyJson["ok"].asBool()
              && denyJson["changed"].asBool(),
          "banctl deny accepts a dynamic rule");
    check(waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/ban", "", bannedIp);
        return response && response->status == 403;
    }, 5000), "dynamic deny synchronizes to Guard");
    auto hubMetrics = apps.hubMetrics();
    check(hubMetrics && promValue(*hubMetrics, "ipban_hub_rules").value_or(0) >= 1,
          "hub metrics reports the injected rule");
    const ToolResult unban = runTool({
        Apps::appBinary("banctl").string(), "--sock", apps.adminSock().string(), "--json",
        "unban", bannedIp
    }, apps.root());
    Json::Value unbanJson;
    check(unban.exitCode == 0 && toolJson(unban, unbanJson) && unbanJson["ok"].asBool()
              && unbanJson["changed"].asBool(),
          "banctl unban accepts the rule removal");
    check(waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/ban", "", bannedIp);
        return response && response->status == 200;
    }, 5000), "unban restores the data path");

    // B4, per-route token bucket rejects then refills
    const std::string rateIp = "198.51.100.42";
    auto rateFirst = apps.gatewayRequest("GET", "/api/rate", "", rateIp);
    auto rateLimited = apps.gatewayRequest("GET", "/api/rate", "", rateIp);
    check(rateFirst && rateFirst->status == 200 && rateLimited && rateLimited->status == 429,
          "route rate limit returns 429 after capacity is spent");
    check(waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/rate", "", rateIp);
        return response && response->status == 200;
    }, 2000), "refilled token bucket restores 200");

    // B5/B6, route strips /api and relays the upstream response
    const size_t proxyStart = apps.mock().requestCount();
    auto proxied = apps.gatewayRequest("POST", "/api/hello", "journey-body", "198.51.100.43");
    check(proxied && proxied->status == 200 && proxied->body == "mock:/hello:journey-body",
          "proxy returns status and body from mock upstream");
    check(apps.mock().waitForRequests(proxyStart + 1, 2000), "mock observes proxied request");
    const auto proxyRecords = apps.mock().records();
    check(proxyRecords.size() > proxyStart && proxyRecords[proxyStart].target == "/hello"
              && proxyRecords[proxyStart].body == "journey-body",
          "strip_prefix reaches upstream as /hello");

    // B7, one upstream fault becomes a gateway error and then recovers
    apps.mock().setMode(MockMode::REJECT);
    auto rejected = apps.gatewayRequest("GET", "/api/reject", "", "198.51.100.44");
    check(rejected && rejected->status == 502, "upstream rejection becomes gateway 502");
    apps.mock().setMode(MockMode::HEALTHY);
    auto recovered = apps.gatewayRequest("GET", "/api/recovered", "", "198.51.100.45");
    check(recovered && recovered->status == 200, "healthy upstream restores proxy success");

    // B8, WAF blocks SQLi before route proxying
    auto waf = apps.gatewayRequest("GET", "/api/search?q=union%20select", "", "203.0.113.56");
    check(waf && waf->status == 403, "WAF returns 403 for SQLi request");

    // C1, a known request batch has an exact stats delta
    const auto statsBefore = apps.stats();
    constexpr uint64_t kStatsRequests = 3;
    bool statsBatchOk = true;
    for(uint64_t i = 0; i < kStatsRequests; ++i) {
        auto response = apps.gatewayRequest("GET", "/api/stats", "",
                                            "198.51.100." + std::to_string(60 + i));
        statsBatchOk = statsBatchOk && response && response->status == 200;
    }
    const auto statsAfter = apps.stats();
    check(statsBatchOk, "known request batch completes");
    check(statsBefore && statsAfter
              && jsonUnsigned(*statsAfter, "requests").value_or(0)
                     == jsonUnsigned(*statsBefore, "requests").value_or(0) + kStatsRequests,
          "stats request counter advances by the known batch size");
    check(statsAfter && jsonUnsigned(*statsAfter, "rate_limited").value_or(0) > 0
              && jsonUnsigned(*statsAfter, "ipban_denied").value_or(0) > 0,
          "stats keeps rate and ban counters after the journey");

    // C2/C4, admin exposes routes then swaps a route and upstream atomically
    check(hasRoute(apps, "journey", "mock"), "routes endpoint exposes live route");
    const auto statsBeforeReload = apps.stats();
    std::string reloadYaml = readFile(apps.gatewayConfigPath());
    check(replaceOnce(reloadYaml, "  - name: mock\n", "  - name: mock-reloaded\n")
              && replaceOnce(reloadYaml, "  - name: journey\n", "  - name: journey-reloaded\n")
              && replaceOnce(reloadYaml, "    upstream: mock\n", "    upstream: mock-reloaded\n")
              && apps.writeRawGateway(reloadYaml, true),
          "write route and upstream reload config");
    auto reload = requestOnce(apps.gatewayAdminPort(), "POST", "/reload");
    const auto statsAfterReload = apps.stats();
    check(reload && reload->status == 200, "admin reload accepts new config");
    check(hasRoute(apps, "journey-reloaded", "mock-reloaded"),
          "routes endpoint exposes reloaded route and upstream");
    check(statsBeforeReload && statsAfterReload
              && jsonUnsigned(*statsAfterReload, "requests").value_or(0)
                     >= jsonUnsigned(*statsBeforeReload, "requests").value_or(0),
          "reload does not reset request stats");
    auto afterReload = apps.gatewayRequest("GET", "/api/reloaded", "", "198.51.100.70");
    check(afterReload && afterReload->status == 200 && afterReload->body == "mock:/reloaded:",
          "reloaded data path still reaches mock upstream");

    // D, TERM drains both children and releases their process resources
    const pid_t gatewayPid = apps.gateway().pid;
    const pid_t hubPid = apps.hub().pid;
    const size_t gatewayFds = fdCount(gatewayPid);
    const size_t hubFds = fdCount(hubPid);
    check(gatewayFds > 0 && hubFds > 0 && threadCount(gatewayPid) > 0 && threadCount(hubPid) > 0,
          "live children expose fd and thread state before shutdown");
    check(apps.stopGateway(), "gateway exits cleanly on TERM");
    check(apps.stopHub(), "hub exits cleanly on TERM");
    check(!alive(apps.gateway()) && !alive(apps.hub()) && fdCount(gatewayPid) == 0
              && fdCount(hubPid) == 0 && threadCount(gatewayPid) == 0 && threadCount(hubPid) == 0,
          "stopped children leave no proc fd or thread entries");

    if(!ok) std::cerr << "test_api_gw_journey diagnostics\n" << apps.diagnostics();
    return TEST_SUMMARY();
}
