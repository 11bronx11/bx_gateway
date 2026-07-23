#include "test_util.h"
#include "test_support.h"

#include <csignal>
#include <iostream>
#include <thread>

using namespace api_gw_test;

namespace {

bool routeNamed(uint16_t adminPort, const std::string& name) {
    auto response = requestOnce(adminPort, "GET", "/routes");
    if(!response || response->status != 200) return false;
    Json::Value routes;
    if(!parseJson(*response, routes) || !routes["routes"].isArray()) return false;
    for(const auto& route : routes["routes"]) {
        if(route["name"].isString() && route["name"].asString() == name) return true;
    }
    return false;
}

int reloadStatus(const Apps& apps) {
    auto response = requestOnce(apps.gatewayAdminPort(), "POST", "/reload");
    return response ? response->status : 0;
}

bool canBind(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return false;
    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

bool canConnect(uint16_t port) {
    const int fd = connectPort(port);
    if(fd < 0) return false;
    ::close(fd);
    return true;
}

bool recvUntil(int fd, std::string& wire, const std::string& needle, Clock::time_point deadline) {
    while(wire.find(needle) == std::string::npos && remainingMs(deadline) > 0) {
        pollfd pfd{fd, POLLIN | POLLHUP, 0};
        if(::poll(&pfd, 1, remainingMs(deadline)) <= 0) return false;
        char buffer[4096];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if(n > 0) {
            wire.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if(n < 0 && errno == EINTR) continue;
        return false;
    }
    return wire.find(needle) != std::string::npos;
}

bool sendExpectContinue(uint16_t port, const std::string& body, HttpResponse& response) {
    const int fd = connectPort(port, 2000);
    if(fd < 0) return false;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    const std::string head = "POST /api/expect HTTP/1.1\r\nHost: client.test\r\n"
        "Content-Length: " + std::to_string(body.size())
        + "\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
    std::string pending;
    bool ok = sendAll(fd, head, deadline)
        && recvUntil(fd, pending, "HTTP/1.1 100 Continue\r\n\r\n", deadline)
        && sendAll(fd, body, deadline)
        && readResponse(fd, pending, response, deadline);
    ::close(fd);
    return ok;
}

std::string withBadUpstreamPort(const std::string& text) {
    const std::string marker = "        port: ";
    const size_t pos = text.find(marker);
    if(pos == std::string::npos) return {};
    const size_t end = text.find('\n', pos);
    if(end == std::string::npos) return {};
    std::string bad = text;
    bad.replace(pos, end - pos, marker + "not-a-port");
    return bad;
}

size_t launchWithWorkers(size_t workers) {
    Apps apps("workers-" + std::to_string(workers), false);
    GatewaySpec spec;
    spec.ioWorkers = workers;
    if(!apps.prepare("workers", spec) || !apps.startGateway() || !apps.waitGatewayReady()) {
        return 0;
    }
    const size_t threads = threadCount(apps.gateway().pid);
    (void)apps.stopGateway();
    return threads;
}

} // namespace

int main() {
    bool ok = true;
    auto check = [&ok](bool value, const std::string& what) {
        TEST_CHECK_MSG(value, what);
        ok = ok && value;
    };

    const size_t oneWorkerThreads = launchWithWorkers(1);
    const size_t threeWorkerThreads = launchWithWorkers(3);
    check(oneWorkerThreads > 0, "fresh one-worker gw starts an IOM");
    check(threeWorkerThreads >= oneWorkerThreads + 2,
          "fresh io_workers=3 creates at least two more process threads than io_workers=1");

    {
        Apps peer("peer-filter", false);
        GatewaySpec spec;
        spec.trustedProxies.clear();
        spec.ipFilterEnabled = true;
        spec.ipCidrs = {"127.0.0.2"};
        check(peer.prepare("peer-filter", spec) && peer.startHub() && peer.waitHubReady()
              && peer.startGateway() && peer.waitGatewayReady(),
              "start isolated peer-IP static filter process");
        const size_t start = peer.mock().requestCount();
        auto forged = peer.gatewayRequest("GET", "/api/forged", "", "127.0.0.2");
        check(forged && forged->status == 200,
              "untrusted peer cannot turn a forged XFF into a static IP-filter denial");
        check(peer.mock().waitForRequests(start + 1, 2000), "peer filter request reaches mock");
        {
            const auto records = peer.mock().records();
            check(records.size() > start && records[start].header("x-forwarded-for") == "127.0.0.1",
                  "untrusted peer proxy rewrite uses TCP peer, not forged XFF");
        }
        auto peerDenied = requestOnceFrom(peer.businessPort(), "127.0.0.2", "GET", "/api/peer");
        check(peerDenied && peerDenied->status == 403,
              "static IP filter separately denies the actual TCP peer");
    }

    {
        Apps trusted("trusted-governance", false);
        GatewaySpec spec;
        spec.ipFilterEnabled = true;
        spec.ipCidrs = {"203.0.113.70"};
        spec.rateLimit = true;
        spec.rateCapacity = 1;
        spec.rateRefillPerSec = 0;
        check(trusted.prepare("trusted-governance", spec) && trusted.setGatewayLogLevel("info")
              && trusted.startHub() && trusted.waitHubReady() && trusted.startGateway()
              && trusted.waitGatewayReady(), "start trusted proxy governance process");
        auto denied = trusted.gatewayRequest("GET", "/api/guard", "", "203.0.113.70");
        check(denied && denied->status == 403,
              "trusted proxy XFF reaches Guard static policy rather than peer-only rule");
        const size_t start = trusted.mock().requestCount();
        auto first = trusted.gatewayRequest("GET", "/api/rate", "", "203.0.113.71");
        auto limited = trusted.gatewayRequest("GET", "/api/rate", "", "203.0.113.71");
        auto other = trusted.gatewayRequest("GET", "/api/rate", "", "203.0.113.72");
        check(first && first->status == 200 && limited && limited->status == 429
              && other && other->status == 200,
              "trusted XFF creates distinct per-route rate-limit buckets");
        check(trusted.mock().waitForRequests(start + 2, 2000),
              "only allowed trusted-proxy requests reach upstream");
        {
            const auto records = trusted.mock().records();
            check(records.size() >= start + 2
                  && records[start].header("x-forwarded-for") == "203.0.113.71"
                  && records[start + 1].header("x-forwarded-for") == "203.0.113.72",
                  "trusted proxy forwarding rewrites XFF from resolved client address");
        }
        check(waitUntil([&] {
            drainNow(trusted.gateway());
            return trusted.gateway().text.find("\"ip\":\"203.0.113.71\"") != std::string::npos;
        }, 2000), "structured access log records the trusted resolved client IP");
        const auto stats = trusted.stats();
        check(stats && jsonUnsigned(*stats, "rate_limited").value_or(0) >= 1
              && jsonUnsigned(*stats, "ipban_denied").value_or(0) >= 1,
              "trusted Guard and rate metrics grow independently");
    }

    {
        Apps maintenance("maintenance", false);
        GatewaySpec spec;
        spec.maintenance = true;
        check(maintenance.prepare("maintenance", spec) && maintenance.startGateway()
              && maintenance.waitGatewayReady(), "start maintenance gateway process");
        const size_t before = maintenance.mock().requestCount();
        auto blocked = maintenance.gatewayRequest("GET", "/api/down", "", "198.51.100.44");
        check(blocked && blocked->status == 503 && blocked->header("Retry-After") == "30",
              "maintenance short-circuits after health and before router/proxy");
        check(maintenance.mock().requestCount() == before, "maintenance request never reaches upstream");
        const auto stats = maintenance.stats();
        check(stats && jsonUnsigned(*stats, "maint_blocked").value_or(0) >= 1,
              "maintenance metric records short circuit");
    }

    Apps apps("lifecycle", false);
    GatewaySpec initial;
    initial.ioWorkers = 1;
    check(apps.prepare("initial", initial), "write isolated YAML and hard-link entries");
    check(apps.startHub(), "start hub process");
    check(apps.waitHubReady(), "hub HTTP and all UDS endpoints are ready");
    check(apps.startGateway(), "start gateway process");
    check(apps.waitGatewayReady(), "gateway healthz is handled by its IOM");

    const pid_t gatewayPid = apps.gateway().pid;
    const uint16_t businessPort = apps.businessPort();
    const uint16_t adminPort = apps.gatewayAdminPort();
    const size_t threadsBeforeReload = threadCount(gatewayPid);
    GatewaySpec changed = initial;
    changed.ioWorkers = 4;
    check(apps.writeGateway("sighup-only", true, changed), "write changed business YAML before SIGHUP");
    check(::kill(gatewayPid, SIGHUP) == 0, "send framework SIGHUP");
    check(waitUntil([&] { return routeNamed(adminPort, "initial"); }, 2000),
          "SIGHUP keeps the old GatewayConfig snapshot");
    check(threadCount(gatewayPid) == threadsBeforeReload,
          "SIGHUP does not rebuild the application IOM workers");

    check(reloadStatus(apps) == 200, "admin POST /reload accepts a new GatewayConfig snapshot");
    check(routeNamed(adminPort, "sighup-only"), "admin reload replaces route snapshot");
    auto afterReload = apps.gatewayRequest("GET", "/api/reload", "", "198.51.100.50");
    check(afterReload && afterReload->status == 200, "same business listener serves after snapshot reload");
    check(apps.gateway().pid == gatewayPid && threadCount(gatewayPid) == threadsBeforeReload,
          "admin reload neither rebinds ports nor changes application io_workers");
    check(canConnect(businessPort) && canConnect(adminPort),
          "business and admin ports remain open after snapshot reload");

    const std::string validYaml = readFile(apps.gatewayConfigPath());
    check(apps.writeRawGateway("server: [\n", true), "write malformed YAML atomically");
    check(reloadStatus(apps) == 500, "malformed YAML rejects admin reload");
    check(routeNamed(adminPort, "sighup-only"), "old snapshot survives malformed YAML");
    auto oldAfterYaml = apps.gatewayRequest("GET", "/api/old-after-yaml", "", "198.51.100.51");
    check(oldAfterYaml && oldAfterYaml->status == 200,
          "old snapshot remains live after malformed YAML");

    const std::string badUpstream = withBadUpstreamPort(validYaml);
    check(!badUpstream.empty() && apps.writeRawGateway(badUpstream, true),
          "write invalid upstream endpoint YAML");
    check(reloadStatus(apps) == 500, "unparseable upstream rejects admin reload");
    check(routeNamed(adminPort, "sighup-only"), "old snapshot survives invalid upstream");
    check(apps.writeGateway("data", true, changed) && reloadStatus(apps) == 200,
          "restore a valid snapshot after failed reloads");

    const size_t normalStart = apps.mock().requestCount();
    auto normal = apps.gatewayRequest("GET", "/api/repeat", "", "198.51.100.60", {
        {"X-Dupe", "one"}, {"X-Dupe", "two"}
    });
    check(normal && normal->status == 200 && !normal->header("X-Request-Id").empty(),
          "request id is generated and normal request proxies");
    check(normal && normal->headerValues("Set-Cookie").size() == 2,
          "repeated upstream response headers survive proxying");
    check(apps.mock().waitForRequests(normalStart + 1, 2000), "mock records normal request");
    {
        const auto records = apps.mock().records();
        if(records.size() <= normalStart || !normal) {
            check(false, "normal mock record is available");
        } else {
            const auto& record = records[normalStart];
            check(record.headerValues("x-dupe").size() == 2,
                  "mock preserves repeated client headers");
            check(record.headers.at("x-request-id") == normal->header("X-Request-Id"),
                  "gateway forwards generated request id upstream");
        }
    }

    GatewaySpec cors = changed;
    cors.extra = "cors:\n  enabled: true\n  allow_origins:\n    - https://client.example\n"
                 "  allow_methods: GET, POST, OPTIONS\n  allow_headers: X-Test\n";
    check(apps.writeGateway("cors", true, cors) && reloadStatus(apps) == 200,
          "reload CORS snapshot");
    const size_t beforePreflight = apps.mock().requestCount();
    auto preflight = apps.gatewayRequest("OPTIONS", "/api/cors", "", "198.51.100.61", {
        {"Origin", "https://client.example"}, {"Access-Control-Request-Method", "POST"}
    });
    check(preflight && preflight->status == 204
          && preflight->header("Access-Control-Allow-Origin") == "https://client.example",
          "CORS preflight short-circuits before router and proxy");
    check(apps.mock().requestCount() == beforePreflight, "preflight does not reach upstream mock");

    apps.mock().setMode(MockMode::CHUNKED);
    auto chunkedResponse = apps.gatewayRequest("GET", "/api/chunked-response", "", "198.51.100.62");
    check(chunkedResponse && chunkedResponse->status == 200
          && chunkedResponse->body == "mock:/chunked-response:",
          "chunked upstream response reaches client as complete body");
    check(chunkedResponse && containsToken(chunkedResponse->header("Transfer-Encoding"), "chunked"),
          "gateway keeps chunked framing for streamed upstream response");

    apps.mock().setMode(MockMode::INFORMATIONAL);
    auto informational = apps.gatewayRequest("GET", "/api/informational", "", "198.51.100.63");
    check(informational && informational->status == 200
          && informational->body == "mock:/informational:",
          "gateway discards upstream 1xx and forwards final response");

    apps.mock().setMode(MockMode::UNTIL_CLOSE);
    auto closeDelimited = apps.gatewayRequest("GET", "/api/close-body", "", "198.51.100.64");
    check(closeDelimited && closeDelimited->status == 200
          && closeDelimited->body == "mock:/close-body:",
          "upstream response without content length is completed on close");
    check(closeDelimited && containsToken(closeDelimited->header("Transfer-Encoding"), "chunked"),
          "close-delimited upstream is safely chunked toward client");

    apps.mock().setMode(MockMode::HEALTHY);
    const size_t chunkedRequestStart = apps.mock().requestCount();
    const std::string chunkedRequest = requestRaw(apps.businessPort(),
        "POST /api/chunk-body HTTP/1.1\r\nHost: client.test\r\nTransfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    check(chunkedRequest.find(" 200 ") != std::string::npos, "chunked client request succeeds");
    check(apps.mock().waitForRequests(chunkedRequestStart + 1, 2000),
          "mock observes chunked client request");
    {
        const auto records = apps.mock().records();
        check(records.size() > chunkedRequestStart && records[chunkedRequestStart].body == "Wikipedia",
              "gateway forwards decoded chunked body to upstream framing");
    }

    HttpResponse expectResponse;
    check(sendExpectContinue(apps.businessPort(), "hello", expectResponse)
          && expectResponse.status == 200 && expectResponse.body == "mock:/expect:hello",
          "Expect 100-continue is emitted before a proxied request body");
    const std::string badExpect = requestRaw(apps.businessPort(),
        "POST /api/bad-expect HTTP/1.1\r\nHost: client.test\r\nContent-Length: 5\r\n"
        "Expect: fancy\r\nConnection: close\r\n\r\nhello");
    check(badExpect.find(" 417 ") != std::string::npos,
          "unsupported Expect token fails with 417 without proxying body");

    GatewaySpec fast = cors;
    fast.totalTimeoutMs = 250;
    fast.readTimeoutMs = 120;
    fast.connectTimeoutMs = 120;
    check(apps.writeGateway("faults", true, fast) && reloadStatus(apps) == 200,
          "reload short upstream deadlines for fault boundaries");
    apps.mock().setMode(MockMode::REJECT);
    auto rejected = apps.gatewayRequest("GET", "/api/reject", "", "198.51.100.65", {}, 1200);
    check(rejected && rejected->status == 502, "accepted upstream rejection becomes 502");
    apps.mock().setMode(MockMode::TIMEOUT_HEAD);
    apps.mock().setDelayMs(500);
    auto timedOut = apps.gatewayRequest("GET", "/api/timeout", "", "198.51.100.66", {}, 1200);
    check(timedOut && timedOut->status == 504, "upstream header timeout becomes 504");
    apps.mock().setDelayMs(0);
    apps.mock().setMode(MockMode::HEALTHY);
    const uint16_t refusedPort = freePort();
    GatewaySpec refused = fast;
    refused.upstreamPort = refusedPort;
    check(refusedPort && apps.writeGateway("refused", true, refused) && reloadStatus(apps) == 200,
          "reload endpoint with no listener");
    auto refusedResponse = apps.gatewayRequest("GET", "/api/refused", "", "198.51.100.67", {}, 1200);
    check(refusedResponse && refusedResponse->status == 502, "connection-refused upstream becomes 502");
    check(apps.writeGateway("stream-errors", true, fast) && reloadStatus(apps) == 200,
          "restore mock upstream after connection refusal");

    apps.mock().setMode(MockMode::PARTIAL_RESPONSE);
    const std::string partial = requestRaw(apps.businessPort(),
        "GET /api/partial HTTP/1.1\r\nHost: client.test\r\nConnection: close\r\n\r\n", 1200);
    check(partial.find("HTTP/1.1 200") != std::string::npos
          && partial.find("mock:/partial:") == std::string::npos,
          "partial upstream body is not reported as a complete client response");
    apps.mock().setMode(MockMode::BAD_CHUNK);
    const std::string badChunk = requestRaw(apps.businessPort(),
        "GET /api/bad-chunk HTTP/1.1\r\nHost: client.test\r\nConnection: close\r\n\r\n", 1200);
    check(badChunk.find("HTTP/1.1 200") != std::string::npos
          && badChunk.find("\r\n0\r\n\r\n") == std::string::npos,
          "bad upstream chunk does not receive a false terminal chunk");
    apps.mock().setMode(MockMode::HEALTHY);

    const auto stats = apps.stats();
    check(stats && jsonUnsigned(*stats, "requests").value_or(0) >= 12,
          "gateway stats requests are monotonic across lifecycle scenarios");
    check(stats && jsonUnsigned(*stats, "upstream_ok").value_or(0) >= 6
          && jsonUnsigned(*stats, "upstream_fail").value_or(0) >= 2,
          "gateway stats separate successful and failed upstream attempts");
    check(stats && (*stats)["routes"].isObject() && (*stats)["routes"].isMember("/api"),
          "route metrics use the source routeKey /api");

    const size_t mockBeforeShutdown = apps.mock().requestCount();
    GatewaySpec slow = initial;
    slow.totalTimeoutMs = 1000;
    slow.readTimeoutMs = 1000;
    check(apps.writeGateway("shutdown", true, slow) && reloadStatus(apps) == 200,
          "reload slow upstream config before TERM");
    HttpClient keepAlive;
    HttpResponse keepAliveResponse;
    check(keepAlive.connect(apps.businessPort())
          && keepAlive.request("GET", "/api/keep", "", {}, true, keepAliveResponse),
          "keep-alive client remains open before shutdown");
    apps.mock().setMode(MockMode::TIMEOUT_HEAD);
    apps.mock().setDelayMs(1500);
    std::thread slowClient([&] {
        (void)apps.gatewayRequest("GET", "/api/slow", "", "198.51.100.68", {}, 3000);
    });
    check(apps.mock().waitForRequests(mockBeforeShutdown + 2, 2000),
          "slow upstream request is in flight before TERM");
    check(apps.stopGateway(8000), "TERM stops gateway with slow and keep-alive clients");
    check(apps.stopHub(), "TERM stops hub with subscription state");
    slowClient.join();
    keepAlive.close();
    check(canBind(businessPort) && canBind(adminPort) && canBind(apps.hubHttpPort()),
          "TERM releases all TCP listeners by deadline");
    check(!fs::exists(apps.submitSock()) && !fs::exists(apps.subscribeSock())
          && !fs::exists(apps.adminSock()), "hub TERM removes all UDS paths");
    check(fs::exists(apps.root() / "hub.db"), "SQLite remains available for next hub start");

    if(!ok) std::cerr << "test_api_gw_lifecycle diagnostics\n" << apps.diagnostics();
    return TEST_SUMMARY();
}
