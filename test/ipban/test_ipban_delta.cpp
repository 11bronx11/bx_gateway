// R2 增量: ChangeLog 窗口 + engine 选 SNAP/DELTA + Guard.applyDelta 版本对账。纯内存。
#include "test_util.h"
#include "engine.h"
#include "guard.h"
#include "changelog.h"
#include "wire.h"
#include "util.h"

using namespace bronx::ipban;

static Ip mkIp(const char* s) { Ip ip; parseCidr(s, ip); return ip; }
static Risk mkRisk(const char* id, const char* ip, uint64_t banMs) {
    Risk r; r.id = id; r.ip = mkIp(ip); r.src = Src::RATE; r.banMs = banMs;
    r.atMs = bronx::GetCurrentMs(); return r;
}

static void testChangeLog() {
    ChangeLog log(3);   // 小环, 测滚动
    DeltaOp op; op.del = false;
    log.append(1, op); log.append(2, op); log.append(3, op);
    TEST_CHECK_EQ(log.oldestVersion(), 1u);
    log.append(4, op);   // 挤掉 v1
    TEST_CHECK_EQ(log.oldestVersion(), 2u);
    TEST_CHECK_EQ(log.size(), 3u);
    // fromVer=1 那条(v2)还在 -> 能覆盖
    TEST_CHECK(log.canCover(1));
    // fromVer=0 要 v1, 已滚出 -> 不能覆盖
    TEST_CHECK(!log.canCover(0));
    std::vector<DeltaOp> out;
    log.collect(2, 4, out);
    TEST_CHECK_EQ(out.size(), 2u);   // v3 v4
}

static void testEngineSnapVsDelta() {
    PolicyEngine eng;
    eng.onRisk(mkRisk("a", "1.1.1.1", 0));   // v1
    eng.onRisk(mkRisk("b", "2.2.2.2", 0));   // v2
    TEST_CHECK_EQ(eng.version(), 2u);

    Kind k; std::string body; uint64_t nv = 0;
    // 没发过 -> 全量
    TEST_CHECK(eng.nextPush(0, false, k, body, nv));
    TEST_CHECK(k == Kind::SNAP);
    TEST_CHECK_EQ(nv, 2u);
    // 全量 body 里应带 engine 的 epoch
    uint64_t sep = 0, sver = 0; std::vector<Rule> srules;
    TEST_CHECK(snapFromJson(body, sep, sver, srules));
    TEST_CHECK_MSG(sep == eng.epoch(), "snap 应带 engine epoch");

    // 发到 v2, 无新东西
    TEST_CHECK(!eng.nextPush(2, true, k, body, nv));

    // 发到 v1, 有 v2 增量 -> DELTA
    eng.onRisk(mkRisk("c", "3.3.3.3", 0));   // v3
    TEST_CHECK(eng.nextPush(1, true, k, body, nv));
    TEST_CHECK_MSG(k == Kind::DELTA, "窗口内应发增量");
    TEST_CHECK_EQ(nv, 3u);
    uint64_t dep = 0, prev = 0, newv = 0; std::vector<DeltaOp> ops;
    TEST_CHECK(deltaFromJson(body, dep, prev, newv, ops));
    TEST_CHECK(dep == eng.epoch());
    TEST_CHECK_EQ(prev, 1u);
    TEST_CHECK_EQ(newv, 3u);
    TEST_CHECK_EQ(ops.size(), 2u);   // v2 v3
    TEST_CHECK_EQ(ops[0].rule.version, 2u);
    TEST_CHECK_EQ(ops[1].rule.version, 3u);

    // sentVer 超前(报了陈旧 epoch 的高版本)应退回全量, 不算负区间
    TEST_CHECK(eng.nextPush(999, true, k, body, nv));
    TEST_CHECK_MSG(k == Kind::SNAP, "sentVer 超前应发全量");
}

static void testGuardApplyDelta() {
    Guard g;
    g.setEnabled(true);
    const uint64_t EP = 111;   // 固定 epoch, 模拟同一个 daemon 实例
    // 先全量到 v2
    Rule a; a.id="a"; a.ip=mkIp("1.1.1.1"); a.action=Act::DENY; a.src=Src::RATE;
    a.priority=rulePriority(Src::RATE,Act::DENY);
    g.applyRemote(EP, 2, {a});
    TEST_CHECK(g.eval(mkIp("1.1.1.1")).action == Act::DENY);
    TEST_CHECK_EQ(g.remoteVersion(), 2u);

    // 增量: v2->v3 加一条 deny 4.4.4.4
    DeltaOp add; add.del=false; add.ruleId="d"; add.rule.id="d";
    add.rule.ip=mkIp("4.4.4.4"); add.rule.action=Act::DENY; add.rule.src=Src::RATE;
    add.rule.priority=rulePriority(Src::RATE,Act::DENY);
    TEST_CHECK_MSG(g.applyDelta(EP, 2, 3, {add}), "版本对得上应应用");
    TEST_CHECK(g.eval(mkIp("4.4.4.4")).action == Act::DENY);
    TEST_CHECK_EQ(g.remoteVersion(), 3u);

    // 增量: v3->v4 删掉 a
    DeltaOp del; del.del=true; del.ruleId="a";
    TEST_CHECK(g.applyDelta(EP, 3, 4, {del}));
    TEST_CHECK_MSG(g.eval(mkIp("1.1.1.1")).action == Act::ALLOW, "删掉后应放行");
    TEST_CHECK(g.eval(mkIp("4.4.4.4")).action == Act::DENY);   // d 还在

    // 版本 gap: 本地 v4, 来个 prev=6 的增量 -> 拒绝(返回 false), 版本不动
    DeltaOp x; x.del=true; x.ruleId="d";
    TEST_CHECK_MSG(!g.applyDelta(EP, 6, 7, {x}), "版本对不上应拒绝");
    TEST_CHECK_EQ(g.remoteVersion(), 4u);
    TEST_CHECK_MSG(g.eval(mkIp("4.4.4.4")).action == Act::DENY, "拒绝的增量不该改状态");

    // epoch 对不上(别的 daemon 实例): 版本对得上也拒
    TEST_CHECK_MSG(!g.applyDelta(EP + 1, 4, 5, {x}), "epoch 对不上应拒绝");
    TEST_CHECK_EQ(g.remoteVersion(), 4u);
    TEST_CHECK(g.remoteEpoch() == EP);
    TEST_CHECK_MSG(!g.applyDelta(EP, 4, 4, {x}), "delta version must move forward");
}

// daemon 重启(epoch 变, 版本回退)网关应无条件收下新全量, 不再被旧版本守卫挡死
static void testEpochReset() {
    Guard g;
    g.setEnabled(true);
    // 旧实例 epoch=100 打到 v50, 封了 1.1.1.1
    g.applyRemote(100, 50, { [](){
        Rule r; r.id="x"; parseCidr("1.1.1.1", r.ip); r.action=Act::DENY;
        r.src=Src::RATE; r.priority=rulePriority(Src::RATE,Act::DENY); return r; }() });
    TEST_CHECK(g.eval(mkIp("1.1.1.1")).action == Act::DENY);
    TEST_CHECK_EQ(g.remoteVersion(), 50u);

    // 同实例来个低版本 v3 -> 旧守卫挡下, 状态不动
    g.applyRemote(100, 3, {});
    TEST_CHECK_MSG(g.eval(mkIp("1.1.1.1")).action == Act::DENY, "同 epoch 低版本应忽略");
    TEST_CHECK_EQ(g.remoteVersion(), 50u);

    // daemon 重启: epoch 变 200, 版本回到 v3, 封了 2.2.2.2 (没了 1.1.1.1)
    g.applyRemote(200, 3, { [](){
        Rule r; r.id="y"; parseCidr("2.2.2.2", r.ip); r.action=Act::DENY;
        r.src=Src::RATE; r.priority=rulePriority(Src::RATE,Act::DENY); return r; }() });
    TEST_CHECK_MSG(g.eval(mkIp("1.1.1.1")).action == Act::ALLOW, "重启后旧规则应清掉");
    TEST_CHECK_MSG(g.eval(mkIp("2.2.2.2")).action == Act::DENY, "重启后新规则应生效");
    TEST_CHECK_EQ(g.remoteVersion(), 3u);
    TEST_CHECK(g.remoteEpoch() == 200u);
}

int main() {
    testChangeLog();
    testEngineSnapVsDelta();
    testGuardApplyDelta();
    testEpochReset();
    return TEST_SUMMARY();
}
