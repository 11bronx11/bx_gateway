#include "test_util.h"
#include "test_support.h"

#include <iostream>

using namespace api_gw_test;

static bool toolJson(const ToolResult& result, Json::Value& value) {
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in(result.output);
    return Json::parseFromStream(builder, in, &value, &errors);
}

static bool routeNamed(uint16_t adminPort, const std::string& name) {
    auto response = requestOnce(adminPort, "GET", "/routes");
    if(!response || response->status != 200) return false;
    Json::Value routes;
    if(!parseJson(*response, routes) || !routes["routes"].isArray()) return false;
    for(const auto& route : routes["routes"]) {
        if(route["name"].isString() && route["name"].asString() == name) return true;
    }
    return false;
}

int main() {
    bool ok = true;
    auto check = [&ok](bool value, const std::string& what) {
        TEST_CHECK_MSG(value, what);
        ok = ok && value;
    };

    Apps apps("chain", true);
    check(apps.prepare(), "write isolated configs and allocate listeners");
    check(apps.startHub(), "launch hub");
    check(apps.waitHubReady(), "hub healthz and UDS listeners ready");
    check(apps.startGateway(), "launch gw");
    check(apps.waitGatewayReady(), "gateway healthz ready");

    auto get = apps.gatewayRequest("GET", "/api/ping", "", "198.51.100.10");
    check(get && get->status == 200, "GET reaches external TCP upstream");
    check(get && get->body == "mock:/ping:", "GET strip_prefix response body");

    auto post = apps.gatewayRequest("POST", "/api/echo", "payload=chain", "198.51.100.10");
    check(post && post->status == 200, "POST reaches external TCP upstream");
    check(post && post->body == "mock:/echo:payload=chain", "POST response body");
    if(!get || !post || get->status != 200 || post->status != 200) {
        std::cerr << "initial GET raw:\n" << (get ? get->raw : "<no response>") << "\n"
                  << "initial POST raw:\n" << (post ? post->raw : "<no response>") << "\n"
                  << apps.diagnostics();
        return TEST_SUMMARY();
    }
    check(apps.mock().waitForRequests(2, 2000), "mock observed normal requests");
    {
        const auto records = apps.mock().records();
        check(records.size() >= 2 && records[0].method == "GET" && records[0].target == "/ping",
              "mock records stripped GET target");
        check(records.size() >= 2 && records[1].method == "POST"
              && records[1].body == "payload=chain", "mock records POST body");
    }

    const size_t forwardingStart = apps.mock().requestCount();
    auto forwarded = apps.gatewayRequest("GET", "/api/headers", "", "198.51.100.11", {
        {"Connection", "close, X-Client-Hop"},
        {"Proxy-Connection", "client-private"},
        {"X-Client-Hop", "client-private"},
        {"X-Real-IP", "forged"},
        {"X-Forwarded-Host", "forged.example"},
        {"X-Forwarded-Proto", "ssh"},
    });
    check(forwarded && forwarded->status == 200, "forwarding header request succeeds");
    check(forwarded && forwarded->header("Proxy-Connection").empty(),
          "client does not receive upstream Proxy-Connection");
    check(forwarded && forwarded->header("X-Up-Hop").empty(),
          "client does not receive upstream Connection-token header");
    check(forwarded && forwarded->header("X-Forwarded-For").empty(),
          "client does not receive upstream forwarded header");
    check(apps.mock().waitForRequests(forwardingStart + 1, 2000),
          "mock observes forwarding header request");
    {
        const auto records = apps.mock().records();
        if(records.size() <= forwardingStart) {
            check(false, "forwarding record is present");
        } else {
            const auto& record = records[forwardingStart];
            const auto header = [&record](const char* name) {
                auto it = record.headers.find(name);
                return it == record.headers.end() ? std::string{} : it->second;
            };
            check(header("proxy-connection").empty(), "upstream does not receive Proxy-Connection");
            check(header("x-client-hop").empty(), "upstream does not receive client Connection token");
            check(header("x-forwarded-for") == "198.51.100.11", "gateway rewrites X-Forwarded-For");
            check(header("x-real-ip") == "198.51.100.11", "gateway rewrites X-Real-IP");
            check(header("x-forwarded-host") == "client.test", "gateway rewrites X-Forwarded-Host");
            check(header("x-forwarded-proto") == "http", "gateway rewrites X-Forwarded-Proto");
            check(header("connection") == "keep-alive",
                  "upstream gets gateway controlled keep-alive, not client Connection value");
        }
    }

    apps.mock().setHealthy(false);
    auto failed = apps.gatewayRequest("GET", "/api/unavailable", "", "198.51.100.12");
    check(failed && failed->status == 502, "mock fail mode surfaces gateway upstream failure");
    apps.mock().setHealthy(true);
    auto recovered = apps.gatewayRequest("GET", "/api/recovered", "", "198.51.100.12");
    check(recovered && recovered->status == 200, "mock healthy mode recovers");

    const std::string wafIp = "203.0.113.31";
    auto waf = apps.gatewayRequest("GET", "/api/search?q=union%20select", "", wafIp);
    check(waf && waf->status == 403, "WAF immediately denies SQLi request");
    const bool syncedWafDeny = waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/normal", "", wafIp);
        return response && response->status == 403;
    }, 6000);
    check(syncedWafDeny, "Reporter, hub submit, subscribe and Guard deny same XFF");
    auto otherIp = apps.gatewayRequest("GET", "/api/normal", "", "203.0.113.32");
    check(otherIp && otherIp->status == 200, "other XFF remains allowed");
    const bool wafExpired = waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/normal", "", wafIp);
        return response && response->status == 200;
    }, 7000);
    check(wafExpired, "WAF remote TTL expires and original XFF recovers");

    const std::string adminIp = "203.0.113.61";
    const ToolResult deny = runTool({
        Apps::appBinary("banctl").string(), "--sock", apps.adminSock().string(), "--json",
        "deny", adminIp, "perm", "chain-admin"
    }, apps.root());
    Json::Value denyJson;
    check(deny.exitCode == 0, "banctl deny exit code");
    check(toolJson(deny, denyJson) && denyJson["ok"].asBool() && denyJson["changed"].asBool(),
          "banctl deny JSON response");
    check(waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/admin", "", adminIp);
        return response && response->status == 403;
    }, 5000), "banctl deny synchronizes to gateway Guard");

    auto metrics = apps.hubMetrics();
    check(metrics.has_value(), "hub metrics endpoint");
    check(metrics && promValue(*metrics, "ipban_hub_rules").value_or(0) >= 1,
          "hub metrics parses persisted rule gauge");
    check(metrics && promSum(*metrics, "ipban_hub_changes_total{") >= 1,
          "hub metrics parses rule change counters");

    const ToolResult unban = runTool({
        Apps::appBinary("banctl").string(), "--sock", apps.adminSock().string(), "--json",
        "unban", adminIp
    }, apps.root());
    Json::Value unbanJson;
    check(unban.exitCode == 0, "banctl unban exit code");
    check(toolJson(unban, unbanJson) && unbanJson["ok"].asBool() && unbanJson["changed"].asBool(),
          "banctl unban JSON response");
    check(waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/admin", "", adminIp);
        return response && response->status == 200;
    }, 5000), "banctl unban restores gateway request");

    const std::string restartIp = "203.0.113.71";
    const ToolResult persistentDeny = runTool({
        Apps::appBinary("banctl").string(), "--sock", apps.adminSock().string(), "--json",
        "deny", restartIp, "perm", "restart-admin"
    }, apps.root());
    check(persistentDeny.exitCode == 0, "persistent banctl deny exit code");
    check(waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/restart", "", restartIp);
        return response && response->status == 403;
    }, 5000), "persistent admin rule reaches gateway before hub restart");
    const auto beforeRestart = apps.stats();
    const uint64_t oldConnects = beforeRestart
        ? jsonUnsigned(*beforeRestart, "ipban_connects").value_or(0) : 0;

    check(apps.stopHub(), "stop hub for restart");
    check(apps.startHub(), "restart hub from same SQLite database");
    check(apps.waitHubReady(), "restarted hub healthz and UDS ready");
    check(waitUntil([&] {
        auto stats = apps.stats();
        return stats && jsonUnsigned(*stats, "ipban_synced").value_or(0) == 1
            && jsonUnsigned(*stats, "ipban_connects").value_or(0) > oldConnects;
    }, 7000), "gateway reconnects and resynchronizes after hub restart");
    check(waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/restart", "", restartIp);
        return response && response->status == 403;
    }, 3000), "SQLite rule remains effective after hub restart");

    const ToolResult persistentUnban = runTool({
        Apps::appBinary("banctl").string(), "--sock", apps.adminSock().string(), "--json",
        "unban", restartIp
    }, apps.root());
    check(persistentUnban.exitCode == 0, "post-restart banctl unban exit code");
    check(waitUntil([&] {
        auto response = apps.gatewayRequest("GET", "/api/restart", "", restartIp);
        return response && response->status == 200;
    }, 5000), "post-restart unban restores request");

    check(apps.writeGateway("api-reloaded", true), "atomically write reload config");
    auto reload = requestOnce(apps.gatewayAdminPort(), "POST", "/reload");
    check(reload && reload->status == 200, "gateway admin reload");
    check(routeNamed(apps.gatewayAdminPort(), "api-reloaded"), "gateway reload exposes new route config");
    auto afterReload = apps.gatewayRequest("GET", "/api/reload", "", "198.51.100.90");
    check(afterReload && afterReload->status == 200, "gateway business listener remains stable after reload");

    const auto stats = apps.stats();
    check(stats.has_value(), "gateway stats JSON endpoint");
    check(stats && jsonUnsigned(*stats, "requests").value_or(0) >= 12,
          "stats JSON request count grows");
    check(stats && jsonUnsigned(*stats, "upstream_ok").value_or(0) >= 6,
          "stats JSON upstream success count grows");
    check(stats && jsonUnsigned(*stats, "waf_denied").value_or(0) >= 1,
          "stats JSON WAF denial count grows");
    check(stats && jsonUnsigned(*stats, "risk_sent").value_or(0) >= 1,
          "stats JSON Reporter send count grows");
    check(stats && jsonUnsigned(*stats, "ipban_denied").value_or(0) >= 3,
          "stats JSON Guard denial count grows");

    if(!ok) std::cerr << "test_api_gw_chain diagnostics\n" << apps.diagnostics();
    return TEST_SUMMARY();
}
