// 阶段2 网关数据面: Snap 编译 + eval 优先级 + Guard 静态/远程合并 + 本地过期兜底。
// 纯内存逻辑, 不碰网络。
#include "test_util.h"
#include "guard.h"
#include "snap.h"
#include "util.h"
#include <thread>

using namespace bronx::ipban;

static Ip mkIp(const char* s) { Ip ip; parseCidr(s, ip); return ip; }

static Rule mkRule(const char* id, const char* cidr, Act a, Src src,
                   uint64_t expireAt = 0) {
    Rule r;
    r.id = id;
    parseCidr(cidr, r.ip);
    r.action = a;
    r.src = src;
    r.priority = rulePriority(src, a);
    r.expireAtMs = expireAt;
    return r;
}

static void testSnapExact() {
    std::vector<Rule> rules = {
        mkRule("r1", "1.2.3.4", Act::DENY, Src::RATE),
        mkRule("r2", "2001:db8::1", Act::DENY, Src::WAF),
    };
    auto snap = compileSnap(1, Act::ALLOW, rules, 0);
    // v4 精确命中
    auto d = snap->eval(mkIp("1.2.3.4"), 0);
    TEST_CHECK(d.action == Act::DENY);
    TEST_CHECK(d.matched);
    TEST_CHECK_EQ(d.ruleId, std::string("r1"));
    // v6 精确命中
    auto d6 = snap->eval(mkIp("2001:db8::1"), 0);
    TEST_CHECK(d6.action == Act::DENY);
    // 没命中走默认
    auto da = snap->eval(mkIp("9.9.9.9"), 0);
    TEST_CHECK(da.action == Act::ALLOW);
    TEST_CHECK(!da.matched);
}

static void testSnapCidr() {
    std::vector<Rule> rules = {
        mkRule("net", "10.0.0.0/8", Act::DENY, Src::RATE),
    };
    auto snap = compileSnap(1, Act::ALLOW, rules, 0);
    TEST_CHECK(snap->eval(mkIp("10.1.2.3"), 0).action == Act::DENY);
    TEST_CHECK(snap->eval(mkIp("11.0.0.1"), 0).action == Act::ALLOW);
}

static void testPriorityArbitration() {
    // 静态 allow 豁免 rate deny(同一 IP)
    std::vector<Rule> rules = {
        mkRule("rate", "1.2.3.4", Act::DENY, Src::RATE),      // prio 400
        mkRule("allow", "1.2.3.4", Act::ALLOW, Src::STATIC),  // prio 750, 更高
    };
    auto snap = compileSnap(1, Act::DENY, rules, 0);
    auto d = snap->eval(mkIp("1.2.3.4"), 0);
    TEST_CHECK_MSG(d.action == Act::ALLOW, "static allow 应压过 rate deny");
    TEST_CHECK_EQ(d.ruleId, std::string("allow"));

    // 紧急 deny 覆盖 admin allow
    std::vector<Rule> r2 = {
        mkRule("aallow", "5.6.7.8", Act::ALLOW, Src::ADMIN),  // 800
        mkRule("emerg", "5.6.7.8", Act::DENY, Src::EMERG),    // 1000
    };
    auto snap2 = compileSnap(1, Act::ALLOW, r2, 0);
    TEST_CHECK(snap2->eval(mkIp("5.6.7.8"), 0).action == Act::DENY);

    // CIDR 高优先压过精确低优先
    std::vector<Rule> r3 = {
        mkRule("exact", "1.2.3.4", Act::DENY, Src::RATE),     // 400 精确
        mkRule("netallow", "1.2.3.0/24", Act::ALLOW, Src::STATIC), // 750 网段
    };
    auto snap3 = compileSnap(1, Act::DENY, r3, 0);
    auto d3 = snap3->eval(mkIp("1.2.3.4"), 0);
    TEST_CHECK_MSG(d3.action == Act::ALLOW, "高优先网段应压过低优先精确");
}

static void testExactFallback() {
    auto v4 = compileSnap(1, Act::ALLOW, {
        mkRule("admin", "1.2.3.4", Act::ALLOW, Src::ADMIN, 200),
        mkRule("waf", "1.2.3.4", Act::DENY, Src::WAF, 1000),
    }, 100);
    TEST_CHECK_EQ(v4->ruleCount(), 2u);
    TEST_CHECK_EQ(v4->eval(mkIp("1.2.3.4"), 150).ruleId, std::string("admin"));
    TEST_CHECK_EQ(v4->eval(mkIp("1.2.3.4"), 250).ruleId, std::string("waf"));

    auto v6 = compileSnap(2, Act::DENY, {
        mkRule("admin", "2001:db8::1", Act::DENY, Src::ADMIN, 200),
        mkRule("static", "2001:db8::1", Act::ALLOW, Src::STATIC),
    }, 100);
    TEST_CHECK_EQ(v6->ruleCount(), 2u);
    TEST_CHECK_EQ(v6->eval(mkIp("2001:db8::1"), 150).ruleId, std::string("admin"));
    TEST_CHECK_EQ(v6->eval(mkIp("2001:db8::1"), 250).ruleId, std::string("static"));
}

static void testExpiry() {
    uint64_t now = bronx::GetCurrentMs();
    std::vector<Rule> rules = {
        mkRule("live", "1.2.3.4", Act::DENY, Src::RATE, now + 100000),  // 还没到
        mkRule("dead", "5.6.7.8", Act::DENY, Src::RATE, now - 1000),    // 已过期
    };
    auto snap = compileSnap(1, Act::ALLOW, rules, now);
    // 编译期就该剔掉 dead
    TEST_CHECK(snap->eval(mkIp("5.6.7.8"), now).action == Act::ALLOW);
    TEST_CHECK(snap->eval(mkIp("1.2.3.4"), now).action == Act::DENY);
    // eval 二次兜底: 传一个 live 也过期的未来时刻
    TEST_CHECK_MSG(snap->eval(mkIp("1.2.3.4"), now + 200000).action == Act::ALLOW,
                   "eval 应二次剔掉过期规则");
}

static void testGuard() {
    Guard g;
    g.setEnabled(true);
    g.setDefaultAction(Act::ALLOW);
    // 关着时一律放行
    Guard off;
    TEST_CHECK(off.eval(mkIp("1.2.3.4")).action == Act::ALLOW);

    // 静态规则
    g.setStatic({ mkRule("s1", "1.2.3.4", Act::DENY, Src::STATIC) });
    TEST_CHECK(g.eval(mkIp("1.2.3.4")).action == Act::DENY);
    TEST_CHECK(g.eval(mkIp("1.2.3.5")).action == Act::ALLOW);

    // 远程规则叠加(不冲掉静态)。EP 固定, 模拟同一 daemon 实例
    const uint64_t EP = 9;
    g.applyRemote(EP, 10, { mkRule("rem", "8.8.8.8", Act::DENY, Src::WAF) });
    TEST_CHECK(g.eval(mkIp("8.8.8.8")).action == Act::DENY);
    TEST_CHECK_MSG(g.eval(mkIp("1.2.3.4")).action == Act::DENY, "远程更新不该清掉静态");
    TEST_CHECK(g.remoteVersion() == 10u);

    // 旧版本远程被忽略
    g.applyRemote(EP, 5, {});
    TEST_CHECK_MSG(g.eval(mkIp("8.8.8.8")).action == Act::DENY, "旧版本应忽略");
    TEST_CHECK(g.remoteVersion() == 10u);

    // 新版本清空远程
    g.applyRemote(EP, 11, {});
    TEST_CHECK_MSG(g.eval(mkIp("8.8.8.8")).action == Act::ALLOW, "新版本空快照应清远程");
    TEST_CHECK_MSG(g.eval(mkIp("1.2.3.4")).action == Act::DENY, "静态仍在");

    // reload 换静态不动远程
    g.applyRemote(EP, 12, { mkRule("rem2", "8.8.8.8", Act::DENY, Src::WAF) });
    g.setStatic({ mkRule("s2", "7.7.7.7", Act::DENY, Src::STATIC) });
    TEST_CHECK_MSG(g.eval(mkIp("8.8.8.8")).action == Act::DENY, "换静态不该清远程");
    TEST_CHECK(g.eval(mkIp("7.7.7.7")).action == Act::DENY);
    TEST_CHECK_MSG(g.eval(mkIp("1.2.3.4")).action == Act::ALLOW, "旧静态应被换掉");
}

static void testLocalExpiryTick() {
    Guard g;
    g.setEnabled(true);
    uint64_t now = bronx::GetCurrentMs();
    // 一条 50ms 后过期的远程规则
    g.applyRemote(1, 1, { mkRule("t", "1.2.3.4", Act::DENY, Src::RATE, now + 50) });
    TEST_CHECK(g.eval(mkIp("1.2.3.4")).action == Act::DENY);
    // 没到期 tick 不该重编
    TEST_CHECK(!g.tickExpiry());
    // 等过期
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    TEST_CHECK_MSG(g.tickExpiry(), "过期后 tick 应重编");
    TEST_CHECK_MSG(g.eval(mkIp("1.2.3.4")).action == Act::ALLOW, "本地兜底应放行");
    TEST_CHECK_EQ(g.remoteCount(), 0u);
}

static void testStaticAndRemoteSwitches() {
    Guard g;
    Rule r = mkRule("remote", "6.6.6.6", Act::DENY, Src::RATE);
    g.setRemoteEnabled(true);
    g.applyRemote(7, 1, {r});
    TEST_CHECK(g.eval(r.ip).action == Act::DENY);

    g.setEnabled(false);
    TEST_CHECK_MSG(g.eval(r.ip).action == Act::DENY, "config reload must not disable remote rules");

    g.setRemoteEnabled(false);
    TEST_CHECK(g.eval(r.ip).action == Act::ALLOW);
}

// applyStatic 一把换默认+规则, 中间没有半拉状态
static void testApplyStaticAtomic() {
    Guard g;
    g.setEnabled(true);
    // denylist: 默认放行, 命中黑名单拒
    g.applyStatic(Act::ALLOW, { mkRule("b", "1.2.3.4", Act::DENY, Src::STATIC) });
    TEST_CHECK(g.eval(mkIp("1.2.3.4")).action == Act::DENY);
    TEST_CHECK(g.eval(mkIp("9.9.9.9")).action == Act::ALLOW);
    // 切 allowlist: 默认拒, 白名单内放行。一把换, 不该有"新默认配旧规则"的窗口
    g.applyStatic(Act::DENY, { mkRule("w", "10.0.0.0/8", Act::ALLOW, Src::STATIC) });
    TEST_CHECK_MSG(g.eval(mkIp("10.1.1.1")).action == Act::ALLOW, "白名单内放行");
    TEST_CHECK_MSG(g.eval(mkIp("1.2.3.4")).action == Act::DENY, "旧黑名单规则应被换掉, 走默认拒");
}

static void testDeltaBadId() {
    Guard g;
    g.setRemoteEnabled(true);
    g.applyRemote(1, 1, {});
    std::vector<DeltaOp> ops;
    DeltaOp good;  good.del = false; good.ruleId = "ok";
    good.rule = mkRule("ok", "1.2.3.4", Act::DENY, Src::WAF);
    DeltaOp bad;   bad.del = false;  bad.ruleId = "";   // 空 id
    bad.rule = mkRule("", "5.6.7.8", Act::DENY, Src::WAF);
    ops.push_back(good);
    ops.push_back(bad);
    TEST_CHECK(!g.applyDelta(1, 1, 2, ops));
    TEST_CHECK(g.remoteVersion() == 1u);
    TEST_CHECK(g.eval(mkIp("1.2.3.4")).action == Act::ALLOW);

    bad.ruleId = "ok";
    bad.rule = mkRule("other", "5.6.7.8", Act::DENY, Src::WAF);
    TEST_CHECK(!g.applyDelta(1, 1, 2, {bad}));
    TEST_CHECK(g.remoteVersion() == 1u);

    TEST_CHECK(g.applyDelta(1, 1, 2, {good}));
    TEST_CHECK(g.eval(mkIp("1.2.3.4")).action == Act::DENY);

    Guard full;
    full.setRemoteEnabled(true);
    full.applyRemote(1, 1, {good.rule, mkRule("", "5.6.7.8", Act::DENY, Src::WAF)});
    TEST_CHECK(full.remoteCount() == 1u);
    TEST_CHECK(full.eval(mkIp("5.6.7.8")).action == Act::ALLOW);
}

// 过期缓存: rebuild 后 tick 仍能按最近过期时刻收工
static void testExpiryCacheRebuild() {
    Guard g;
    g.setEnabled(true);
    uint64_t now = bronx::GetCurrentMs();
    // 先放一条永久静态, 再叠一条 50ms 后过期的远程
    g.setStatic({ mkRule("perm", "1.1.1.1", Act::DENY, Src::STATIC) });
    g.applyRemote(1, 1, { mkRule("tmp", "2.2.2.2", Act::DENY, Src::RATE, now + 50) });
    TEST_CHECK(!g.tickExpiry());   // 还没到
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    TEST_CHECK_MSG(g.tickExpiry(), "过期后应重编");
    TEST_CHECK(g.eval(mkIp("2.2.2.2")).action == Act::ALLOW);
    TEST_CHECK_MSG(g.eval(mkIp("1.1.1.1")).action == Act::DENY, "永久规则不受影响");
    // 全清过期后再 tick 应无活干
    TEST_CHECK(!g.tickExpiry());
}

static void testStaticCfg() {
    // denylist
    StaticFilterCfg deny;
    deny.enabled = true; deny.mode = "denylist";
    deny.cidrs = {"1.2.3.0/24", "9.9.9.9"};
    Act def;
    auto rules = staticRulesFromCfg(deny, def);
    TEST_CHECK(def == Act::ALLOW);
    TEST_CHECK_EQ(rules.size(), 2u);

    Guard g; g.setEnabled(true);
    g.setDefaultAction(def);
    g.setStatic(rules);
    TEST_CHECK(g.eval(mkIp("1.2.3.55")).action == Act::DENY);
    TEST_CHECK(g.eval(mkIp("2.2.2.2")).action == Act::ALLOW);

    // 非法 cidr 混进来, 跳过并计数, 好的照收
    StaticFilterCfg dirty;
    dirty.enabled = true; dirty.mode = "denylist";
    dirty.cidrs = {"1.2.3.0/24", "not-an-ip", "300.0.0.1", "5.5.5.5"};
    Act def3;
    size_t skipped = 0;
    auto rules3 = staticRulesFromCfg(dirty, def3, &skipped);
    TEST_CHECK_MSG(skipped == 2u, "两条非法 cidr 应被计入 skipped");
    TEST_CHECK_EQ(rules3.size(), 2u);

    // allowlist
    StaticFilterCfg allow;
    allow.enabled = true; allow.mode = "allowlist";
    allow.cidrs = {"10.0.0.0/8"};
    Act def2;
    auto rules2 = staticRulesFromCfg(allow, def2);
    TEST_CHECK(def2 == Act::DENY);
    Guard g2; g2.setEnabled(true);
    g2.setDefaultAction(def2);
    g2.setStatic(rules2);
    TEST_CHECK_MSG(g2.eval(mkIp("10.1.1.1")).action == Act::ALLOW, "白名单内放行");
    TEST_CHECK_MSG(g2.eval(mkIp("8.8.8.8")).action == Act::DENY, "白名单外拒");
}

int main() {
    testSnapExact();
    testSnapCidr();
    testPriorityArbitration();
    testExactFallback();
    testExpiry();
    testGuard();
    testLocalExpiryTick();
    testStaticAndRemoteSwitches();
    testApplyStaticAtomic();
    testDeltaBadId();
    testExpiryCacheRebuild();
    testStaticCfg();
    return TEST_SUMMARY();
}
