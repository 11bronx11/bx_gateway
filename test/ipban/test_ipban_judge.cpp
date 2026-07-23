// 阶段3 daemon 纯逻辑: judge 仲裁 + store + expiry 堆。不碰网络。
#include "test_util.h"
#include "judge.h"
#include "store.h"
#include "expiry.h"
#include "engine.h"
#include <limits>

using namespace bronx::ipban;

static Ip mkIp(const char* s) { Ip ip; parseCidr(s, ip); return ip; }

static void testJudge() {
    Risk r;
    r.id = "e1"; r.ip = mkIp("1.2.3.4"); r.src = Src::RATE;
    r.banMs = 60000; r.reason = "abuse";
    Rule rule = ruleFromRisk(r, 1000);
    TEST_CHECK(rule.action == Act::DENY);
    TEST_CHECK(rule.src == Src::RATE);
    TEST_CHECK_EQ(rule.priority, 400);
    TEST_CHECK(rule.expireAtMs == 61000u);   // now+banMs
    // 同源同 IP 落同一 ruleId
    TEST_CHECK_EQ(rule.id, ruleIdFor(Src::RATE, r.ip));

    // banMs=0 永久
    Risk perm; perm.ip = mkIp("5.6.7.8"); perm.src = Src::ADMIN; perm.banMs = 0;
    Rule pr = ruleFromRisk(perm, 1000);
    TEST_CHECK(pr.expireAtMs == 0u);

    Risk huge; huge.ip = mkIp("6.6.6.6"); huge.banMs = std::numeric_limits<uint64_t>::max();
    Rule hr = ruleFromRisk(huge, 1000);
    TEST_CHECK(hr.expireAtMs == (uint64_t)std::numeric_limits<int64_t>::max());
}

static void testReplace() {
    Ip ip = mkIp("1.2.3.4");
    Rule lowShort;  lowShort.priority = 400; lowShort.expireAtMs = 5000;
    Rule lowLong;   lowLong.priority  = 400; lowLong.expireAtMs  = 9000;
    Rule highShort; highShort.priority = 600; highShort.expireAtMs = 3000;

    // 同优先取更晚过期
    TEST_CHECK_MSG(shouldReplace(lowShort, lowLong), "同优先更晚过期应替换");
    TEST_CHECK_MSG(!shouldReplace(lowLong, lowShort), "同优先更早过期不该缩短");
    // 高优先盖低优先
    TEST_CHECK_MSG(shouldReplace(lowLong, highShort), "高优先应盖低优先");
    // 低优先不能盖高优先(哪怕过期更晚)
    Rule lowLonger; lowLonger.priority = 400; lowLonger.expireAtMs = 99999;
    TEST_CHECK_MSG(!shouldReplace(highShort, lowLonger), "低优先不能盖高优先");
    // 永久(0)算最晚
    Rule permRule;  permRule.priority = 400; permRule.expireAtMs = 0;
    TEST_CHECK_MSG(shouldReplace(lowLong, permRule), "永久应盖有限期(同优先)");
}

static void testStore() {
    MemStore s;
    Rule r; r.id = "a"; r.ip = mkIp("1.1.1.1");
    s.put(r);
    TEST_CHECK_EQ(s.size(), 1u);
    TEST_CHECK(s.find("a") != nullptr);
    TEST_CHECK(s.find("nope") == nullptr);
    // 同 id 覆盖
    Rule r2; r2.id = "a"; r2.ip = mkIp("2.2.2.2");
    s.put(r2);
    TEST_CHECK_EQ(s.size(), 1u);
    TEST_CHECK(s.find("a")->ip == mkIp("2.2.2.2"));
    s.erase("a");
    TEST_CHECK_EQ(s.size(), 0u);
}

static void testExpiry() {
    Expiry e;
    e.push(3000, "c", 1);
    e.push(1000, "a", 1);
    e.push(2000, "b", 1);
    e.push(0, "perm", 1);   // 永久不进堆
    TEST_CHECK_EQ(e.size(), 3u);
    TEST_CHECK(e.earliest() == 1000u);   // 小顶

    // popDue 只弹到期的
    auto due = e.popDue(1500);
    TEST_CHECK_EQ(due.size(), 1u);
    TEST_CHECK_EQ(due[0].ruleId, std::string("a"));
    TEST_CHECK_EQ(e.size(), 2u);

    // 版本随节点带出, 给调用方比对
    auto rest = e.popDue(9999);
    TEST_CHECK_EQ(rest.size(), 2u);
    TEST_CHECK(e.empty());
}

static void testRiskId() {
    PolicyEngine e;
    Risk r;
    r.id = "same-event";
    r.ip = mkIp("7.7.7.7");
    r.src = Src::WAF;
    r.banMs = 60000;
    TEST_CHECK(e.onRisk(r));
    TEST_CHECK_MSG(!e.onRisk(r), "same risk id should cool down");
    TEST_CHECK_EQ(e.version(), 1u);
}

int main() {
    testJudge();
    testReplace();
    testStore();
    testExpiry();
    testRiskId();
    return TEST_SUMMARY();
}
