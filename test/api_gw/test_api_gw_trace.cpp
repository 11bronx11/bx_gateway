#include "test_util.h"
#include "test_support.h"

#include <iostream>
#include <optional>
#include <thread>

using namespace api_gw_test;

namespace {

std::string traceFramework(const fs::path& traceLog) {
    return frameworkConfig()
        + "  - name: trace\n"
          "    level: info\n"
          "    appenders:\n"
          "      - type: BxFileLogAppender\n"
          "        file: " + traceLog.string() + "\n"
          "        async: false\n";
}

bool traceStatus(uint16_t port, Json::Value& value) {
    auto response = requestOnce(port, "GET", "/trace");
    return response && response->status == 200 && parseJson(*response, value);
}

bool traceOn(uint16_t port, const std::string& query) {
    auto response = requestOnce(port, "POST", "/trace/on?" + query);
    return response && response->status == 200;
}

bool traceOff(uint16_t port) {
    auto response = requestOnce(port, "POST", "/trace/off");
    return response && response->status == 200;
}

bool noPostStopLines(const std::string& text) {
    return text.find("cli  body#1") == std::string::npos
        && text.find("cli  bodyend") == std::string::npos
        && text.find("req  done") == std::string::npos;
}

} // namespace

int main() {
    bool ok = true;
    auto check = [&ok](bool value, const std::string& what) {
        TEST_CHECK_MSG(value, what);
        ok = ok && value;
    };

    Apps apps("trace", false);
    const fs::path traceLog = apps.root() / "trace.log";
    check(apps.prepare(), "write isolated trace runtime");
    check(writeText(apps.root() / "gw-bronx.yml", traceFramework(traceLog)),
          "write synchronous trace logger config");
    check(apps.startHub() && apps.waitHubReady(), "start trace hub");
    check(apps.startGateway() && apps.waitGatewayReady(), "start trace gateway");

    Json::Value status;
    check(traceStatus(apps.gatewayAdminPort(), status) && !status["on"].asBool()
              && status["level"].asInt() == 0,
          "trace starts disabled");

    auto badTtl = requestOnce(apps.gatewayAdminPort(), "POST", "/trace/on?ttl=not-a-number");
    check(badTtl && badTtl->status == 400, "non-numeric TTL is rejected");
    auto negativeTtl = requestOnce(apps.gatewayAdminPort(), "POST", "/trace/on?ttl=-1");
    check(negativeTtl && negativeTtl->status == 400, "negative TTL is rejected");
    auto hugeTtl = requestOnce(apps.gatewayAdminPort(), "POST",
                               "/trace/on?ttl=9223372036854776");
    check(hugeTtl && hugeTtl->status == 400, "overflowing TTL is rejected");

    const std::string clientIp = "198.51.100.77";
    const size_t ipStart = readFile(traceLog).size();
    check(traceOn(apps.gatewayAdminPort(),
                  "level=1&ip=" + clientIp + "&path=/api/trace-ip&ttl=30"),
          "enable trace for trusted XFF client");
    auto ipResponse = apps.gatewayRequest("GET", "/api/trace-ip", "", clientIp);
    check(ipResponse && ipResponse->status == 200, "trusted XFF request succeeds");
    check(waitUntil([&] {
        return readFile(traceLog).substr(ipStart).find("GET /api/trace-ip") != std::string::npos;
    }, 1000), "trace IP filter matches trusted XFF client");
    check(traceOff(apps.gatewayAdminPort()), "disable XFF trace");

    const size_t offStart = readFile(traceLog).size();
    check(traceOn(apps.gatewayAdminPort(), "level=1&path=/api/trace-off&ttl=30"),
          "enable delayed-request trace");
    apps.mock().setMode(MockMode::DELAY_BODY);
    apps.mock().setDelayMs(1200);
    const size_t beforeOff = apps.mock().requestCount();
    std::optional<HttpResponse> offResponse;
    std::thread offRequest([&] {
        offResponse = apps.gatewayRequest("GET", "/api/trace-off", "", clientIp, {}, 3000);
    });
    check(apps.mock().waitForRequests(beforeOff + 1, 2000), "delayed request reaches upstream");
    check(traceOff(apps.gatewayAdminPort()), "disable trace while request is in flight");
    offRequest.join();
    apps.mock().setDelayMs(0);
    apps.mock().setMode(MockMode::HEALTHY);
    check(offResponse && offResponse->status == 200, "delayed request completes after trace off");
    check(noPostStopLines(readFile(traceLog).substr(offStart)),
          "trace off suppresses later in-flight request lines");

    const size_t ttlStart = readFile(traceLog).size();
    check(traceOn(apps.gatewayAdminPort(), "level=1&path=/api/trace-ttl&ttl=1"),
          "enable one-second trace");
    apps.mock().setMode(MockMode::DELAY_BODY);
    apps.mock().setDelayMs(1500);
    const size_t beforeTtl = apps.mock().requestCount();
    std::optional<HttpResponse> ttlResponse;
    std::thread ttlRequest([&] {
        ttlResponse = apps.gatewayRequest("GET", "/api/trace-ttl", "", clientIp, {}, 3000);
    });
    check(apps.mock().waitForRequests(beforeTtl + 1, 2000), "TTL request reaches upstream");
    check(waitUntil([&] {
        return traceStatus(apps.gatewayAdminPort(), status) && !status["on"].asBool();
    }, 2500), "TTL expires before delayed body is released");
    ttlRequest.join();
    apps.mock().setDelayMs(0);
    apps.mock().setMode(MockMode::HEALTHY);
    check(ttlResponse && ttlResponse->status == 200, "TTL request completes");
    check(noPostStopLines(readFile(traceLog).substr(ttlStart)),
          "TTL suppresses later in-flight request lines");

    if(!ok) std::cerr << "test_api_gw_trace diagnostics\n" << apps.diagnostics();
    return TEST_SUMMARY();
}
