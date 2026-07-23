#include "test_util.h"
#include "test_support.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>

using namespace api_gw_test;

struct Counts {
    std::atomic<uint64_t> getNew{0};
    std::atomic<uint64_t> getKeepAlive{0};
    std::atomic<uint64_t> postNew{0};
    std::atomic<uint64_t> postKeepAlive{0};
    std::atomic<uint64_t> connectFail{0};
    std::atomic<uint64_t> protocolFail{0};
    std::atomic<uint64_t> non200{0};
    std::mutex sampleMutex;
    std::string sample;

    void saveSample(const HttpResponse& response) {
        std::lock_guard<std::mutex> lock(sampleMutex);
        if(sample.empty()) sample = response.raw;
    }

    uint64_t successful() const {
        return getNew + getKeepAlive + postNew + postKeepAlive;
    }

    uint64_t attempted() const {
        return successful() + connectFail + protocolFail + non200;
    }

    std::string describe() const {
        std::ostringstream out;
        out << "get_new=" << getNew.load()
            << " get_keepalive=" << getKeepAlive.load()
            << " post_new=" << postNew.load()
            << " post_keepalive=" << postKeepAlive.load()
            << " connect_fail=" << connectFail.load()
            << " protocol_fail=" << protocolFail.load()
            << " non200=" << non200.load();
        return out.str();
    }
};

int main() {
    bool ok = true;
    auto check = [&ok](bool value, const std::string& what) {
        TEST_CHECK_MSG(value, what);
        ok = ok && value;
    };

    Apps apps("stress", false);
    check(apps.prepare("stress"), "write isolated stress configs");
    check(apps.startHub(), "launch stress hub");
    check(apps.waitHubReady(), "stress hub ready");
    check(apps.startGateway(), "launch stress gateway");
    check(apps.waitGatewayReady(), "stress gateway ready");

    const size_t fdBefore = fdCount(apps.gateway().pid);
    check(fdBefore > 0, "read baseline gateway fd count");

    constexpr int kThreads = 6;
    constexpr int kRequestsPerThread = 40;
    constexpr uint64_t kExpected = kThreads * kRequestsPerThread;
    Counts counts;
    std::atomic<uint64_t> completed{0};
    std::atomic<bool> begin{false};
    const auto trafficDeadline = Clock::now() + std::chrono::seconds(20);
    std::mutex pauseMutex;
    std::condition_variable pauseCv;
    int paused = 0;
    bool resume = false;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for(int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&, worker] {
            while(!begin.load(std::memory_order_acquire)) std::this_thread::yield();
            HttpClient keepAlive;
            bool connected = false;
            for(int i = 0; i < kRequestsPerThread; ++i) {
                if(Clock::now() >= trafficDeadline) {
                    ++counts.protocolFail;
                    break;
                }
                const bool keep = (i % 2) != 0;
                const bool post = (i % 3) == 0;
                const std::string xff = "198.18." + std::to_string(worker) + "."
                    + std::to_string(10 + (i % 20));
                const std::string target = post ? "/api/stress-post" : "/api/stress-get";
                const std::string body = post ? "worker=" + std::to_string(worker)
                    + "&request=" + std::to_string(i) : "";
                std::vector<std::pair<std::string, std::string>> headers = {
                    {"X-Forwarded-For", xff}
                };

                HttpResponse response;
                bool requestOk = false;
                if(keep) {
                    if(!connected) {
                        connected = keepAlive.connect(apps.businessPort(), 2000);
                        if(!connected) {
                            ++counts.connectFail;
                            ++completed;
                            break;
                        }
                    }
                    requestOk = keepAlive.request(post ? "POST" : "GET", target, body, headers,
                                                  true, response, 3000);
                } else {
                    auto once = requestOnce(apps.businessPort(), post ? "POST" : "GET",
                                            target, body, headers, 3000);
                    if(once) {
                        response = std::move(*once);
                        requestOk = true;
                    }
                }

                if(!requestOk) {
                    ++counts.protocolFail;
                    ++completed;
                    break;
                } else if(response.status != 200
                          || response.body != "mock:" + target.substr(4) + ":" + body) {
                    ++counts.non200;
                    counts.saveSample(response);
                } else if(post && keep) {
                    ++counts.postKeepAlive;
                } else if(post) {
                    ++counts.postNew;
                } else if(keep) {
                    ++counts.getKeepAlive;
                } else {
                    ++counts.getNew;
                }
                ++completed;
                if(i == 7) {
                    std::unique_lock<std::mutex> lock(pauseMutex);
                    ++paused;
                    pauseCv.notify_all();
                    pauseCv.wait(lock, [&] { return resume; });
                }
            }
        });
    }

    begin.store(true, std::memory_order_release);
    check(waitUntil([&] {
        drainNow(apps.gateway());
        drainNow(apps.hub());
        std::lock_guard<std::mutex> lock(pauseMutex);
        return paused == kThreads;
    }, 5000), "stress clients pause with keep-alive connections before control-plane events");

    check(apps.writeGateway("stress", true), "atomically write reload under traffic");
    auto reload = requestOnce(apps.gatewayAdminPort(), "POST", "/reload");
    check(reload && reload->status == 200, "reload succeeds under traffic");

    check(apps.stopHub(), "stop hub under traffic");
    check(apps.startHub(), "restart hub under traffic");
    check(apps.waitHubReady(), "restarted hub ready under traffic");
    {
        std::lock_guard<std::mutex> lock(pauseMutex);
        resume = true;
    }
    pauseCv.notify_all();

    for(auto& worker : workers) worker.join();
    drainNow(apps.gateway());
    drainNow(apps.hub());

    check(completed.load() == kExpected, "all stress requests completed before deadline");
    check(counts.attempted() == kExpected, "request category accounting is complete");
    check(counts.successful() == kExpected, "all non-banned requests return successful responses");
    check(counts.connectFail.load() == 0, "no client connect failures");
    check(counts.protocolFail.load() == 0, "no client hangs or protocol failures");
    check(counts.non200.load() == 0, "no unexpected non-200 or response mismatch");
    check(apps.mock().waitForRequests(kExpected, 5000), "mock received every routed request");
    check(apps.mock().requestCount() == kExpected, "upstream request count has no duplicates");
    check(alive(apps.gateway()), "gateway remains alive after stress");
    check(alive(apps.hub()), "hub remains alive after restart and stress");

    std::optional<Json::Value> finalStats;
    check(waitUntil([&] {
        finalStats = apps.stats();
        return finalStats && (*finalStats)["active_conns"].asInt64() == 0;
    }, 5000), "gateway active_conns returns to zero");
    check(finalStats.has_value(), "read final gateway stats JSON");
    if(finalStats) {
        const auto& routes = (*finalStats)["routes"];
        const uint64_t routeRequests = routes.isObject() && routes.isMember("/api")
            ? routes["/api"]["requests"].asUInt64() : 0;
        check(routeRequests == kExpected,
              "route request count equals external upstream count, got "
              + std::to_string(routeRequests));
        check(jsonUnsigned(*finalStats, "upstream_ok").value_or(0) == kExpected,
              "upstream success count equals stress request count");
    }

    const size_t fdAfter = fdCount(apps.gateway().pid);
    check(fdAfter > 0 && fdAfter <= fdBefore + 12,
          "gateway fd growth stays within explicit bound, before=" + std::to_string(fdBefore)
          + " after=" + std::to_string(fdAfter));

    if(!ok) {
        std::cerr << "test_api_gw_stress counts " << counts.describe() << "\n"
                  << "first unexpected response:\n" << counts.sample << "\n"
                  << apps.diagnostics();
    }
    return TEST_SUMMARY();
}
