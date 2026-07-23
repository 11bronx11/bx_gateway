#include "test_util.h"
#include "engine.h"
#include "guard.h"
#include "log.h"
#include "util.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace bronx::ipban;

static Ip makeIp(int a, int b, int c, int d) {
    Ip ip;
    parseCidr(std::to_string(a) + "." + std::to_string(b) + "."
        + std::to_string(c) + "." + std::to_string(d), ip);
    return ip;
}

int main(int argc, char**) {
    constexpr int writers = 4;
    const int perWriter = argc > 1 ? 100 : 1000;
    const int updates = argc > 1 ? 50 : 1000;
    auto systemLog = BRONX_LOG_NAME("system");
    auto oldLevel = systemLog->getLevel();
    systemLog->setLevel(bronx::BxLogLevel::ERROR);

    PolicyEngine engine;
    std::vector<std::thread> putters;
    for(int w = 0; w < writers; ++w) {
        putters.emplace_back([&engine, w, perWriter]() {
            for(int i = 0; i < perWriter; ++i) {
                Risk risk;
                risk.id = "stress-" + std::to_string(w) + "-" + std::to_string(i);
                risk.ip = makeIp(10, w, i / 256, i % 256);
                risk.src = Src::RATE;
                risk.banMs = 0;
                risk.atMs = bronx::GetCurrentMs();
                engine.onRisk(risk);
            }
        });
    }
    for(auto& t : putters) t.join();

    const uint64_t total = writers * perWriter;
    TEST_CHECK_EQ(engine.version(), total);
    TEST_CHECK_EQ(engine.ruleCount(), (size_t)total);

    Kind kind;
    std::string body;
    uint64_t version = 0;
    TEST_CHECK(engine.nextPush(0, false, kind, body, version));
    TEST_CHECK(kind == Kind::SNAP);
    uint64_t epoch = 0;
    std::vector<Rule> rules;
    TEST_CHECK(snapFromJson(body, epoch, version, rules));
    TEST_CHECK_EQ(rules.size(), (size_t)total);

    Guard guard;
    guard.setRemoteEnabled(true);
    guard.applyRemote(epoch, version, std::move(rules));

    std::atomic<bool> reading{true};
    std::atomic<uint64_t> badReads{0};
    std::vector<std::thread> readers;
    for(int n = 0; n < 6; ++n) {
        readers.emplace_back([&]() {
            Ip fixed = makeIp(10, 0, 0, 0);
            uint32_t rounds = 0;
            while(reading.load(std::memory_order_acquire)) {
                if(guard.eval(fixed).action != Act::DENY) ++badReads;
                if((++rounds & 255) == 0) std::this_thread::yield();
            }
        });
    }

    uint64_t next = version;
    for(int i = 0; i < updates; ++i) {
        DeltaOp op;
        op.rule.id = "update-" + std::to_string(i);
        op.rule.ip = makeIp(172, 16 + i / 256, i % 256, 1);
        op.rule.action = Act::DENY;
        op.rule.src = Src::RATE;
        op.rule.priority = rulePriority(op.rule.src, op.rule.action);
        op.rule.version = next + 1;
        op.ruleId = op.rule.id;
        TEST_CHECK(guard.applyDelta(epoch, next, next + 1, {op}));
        ++next;
    }
    reading.store(false, std::memory_order_release);
    for(auto& t : readers) t.join();

    TEST_CHECK_EQ(badReads.load(), 0u);
    TEST_CHECK_EQ(guard.remoteVersion(), total + updates);
    TEST_CHECK_EQ(guard.remoteCount(), (size_t)(total + updates));
    systemLog->setLevel(oldLevel);
    return TEST_SUMMARY();
}
