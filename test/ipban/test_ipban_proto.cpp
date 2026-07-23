// 阶段1 公共层测试: ip 规范化 + CIDR 匹配 + proto 帧 + wire JSON。
// 不碰网络, 纯内存逻辑, 快。
#include "test_util.h"
#include "ip.h"
#include "rule.h"
#include "proto.h"
#include "wire.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>

using namespace bronx::ipban;

static sockaddr_in mkV4(const char* s) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, s, &a.sin_addr);
    return a;
}
static sockaddr_in6 mkV6(const char* s) {
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    inet_pton(AF_INET6, s, &a.sin6_addr);
    return a;
}

static void testIp() {
    // v4 从 sockaddr
    auto s4 = mkV4("1.2.3.4");
    Ip ip;
    TEST_CHECK(ipFromSockaddr((sockaddr*)&s4, ip));
    TEST_CHECK(ip.fam == Fam::V4);
    TEST_CHECK_EQ(ip.prefix, 32);
    TEST_CHECK_EQ(ip.bytes[0], 1); TEST_CHECK_EQ(ip.bytes[3], 4);

    // v6 从 sockaddr
    auto s6 = mkV6("2001:db8::1");
    Ip ip6;
    TEST_CHECK(ipFromSockaddr((sockaddr*)&s6, ip6));
    TEST_CHECK(ip6.fam == Fam::V6);
    TEST_CHECK_EQ(ip6.prefix, 128);

    // v4-mapped 必须归到 v4
    auto sm = mkV6("::ffff:5.6.7.8");
    Ip ipm;
    TEST_CHECK(ipFromSockaddr((sockaddr*)&sm, ipm));
    TEST_CHECK_MSG(ipm.fam == Fam::V4, "v4-mapped 应归 v4");
    TEST_CHECK_EQ(ipm.bytes[0], 5); TEST_CHECK_EQ(ipm.bytes[3], 8);

    Ip parsed;
    TEST_CHECK(parseCidr("::ffff:5.6.7.8", parsed));
    TEST_CHECK_MSG(parsed == ipm, "文本 v4-mapped 也应归 v4");
    TEST_CHECK(parseCidr("::ffff:5.6.7.8/120", parsed));
    Ip mappedNet;
    TEST_CHECK(parseCidr("5.6.7.0/24", mappedNet));
    TEST_CHECK_MSG(parsed == mappedNet, "mapped 网段应归 v4 网段");

    // 未知族
    sockaddr_un su{}; su.sun_family = AF_UNIX;
    Ip ipx;
    TEST_CHECK(!ipFromSockaddr((sockaddr*)&su, ipx));
}

static void testCidr() {
    Ip net, addr;
    // v4 /24
    TEST_CHECK(parseCidr("192.168.1.0/24", net));
    TEST_CHECK_EQ(net.prefix, 24);
    { auto a = mkV4("192.168.1.99"); ipFromSockaddr((sockaddr*)&a, addr); }
    TEST_CHECK_MSG(inNet(addr, net), "192.168.1.99 应在 /24 内");
    { auto a = mkV4("192.168.2.1"); ipFromSockaddr((sockaddr*)&a, addr); }
    TEST_CHECK_MSG(!inNet(addr, net), "192.168.2.1 不在 /24 内");

    // 单 IP(满位)
    Ip single;
    TEST_CHECK(parseCidr("10.0.0.5", single));
    TEST_CHECK_EQ(single.prefix, 32);
    { auto a = mkV4("10.0.0.5"); ipFromSockaddr((sockaddr*)&a, addr); }
    TEST_CHECK(inNet(addr, single));
    { auto a = mkV4("10.0.0.6"); ipFromSockaddr((sockaddr*)&a, addr); }
    TEST_CHECK(!inNet(addr, single));

    // /0 匹配一切 v4
    Ip all;
    TEST_CHECK(parseCidr("0.0.0.0/0", all));
    { auto a = mkV4("8.8.8.8"); ipFromSockaddr((sockaddr*)&a, addr); }
    TEST_CHECK_MSG(inNet(addr, all), "/0 应匹配一切 v4");

    // v6 /32
    Ip net6, addr6;
    TEST_CHECK(parseCidr("2001:db8::/32", net6));
    { auto a = mkV6("2001:db8:dead:beef::1"); ipFromSockaddr((sockaddr*)&a, addr6); }
    TEST_CHECK_MSG(inNet(addr6, net6), "2001:db8:... 应在 /32 内");
    { auto a = mkV6("2001:db9::1"); ipFromSockaddr((sockaddr*)&a, addr6); }
    TEST_CHECK(!inNet(addr6, net6));

    // v4 地址不该匹配 v6 网段
    { auto a = mkV4("1.1.1.1"); ipFromSockaddr((sockaddr*)&a, addr); }
    TEST_CHECK_MSG(!inNet(addr, net6), "跨族不匹配");

    // 非法 prefix
    Ip bad;
    TEST_CHECK(!parseCidr("1.2.3.4/33", bad));
    TEST_CHECK(!parseCidr("1.2.3.4/-1", bad));
    TEST_CHECK(!parseCidr("2001:db8::/129", bad));
    TEST_CHECK(!parseCidr("notanip", bad));
    TEST_CHECK(!parseCidr("1.2.3.4/", bad));

    // 网段 bytes 应已按掩码清主机位
    Ip masked;
    TEST_CHECK(parseCidr("192.168.1.77/24", masked));
    TEST_CHECK_EQ(masked.bytes[3], 0);
}

static void testHead() {
    Head h;
    h.kind = (uint16_t)Kind::SNAP;
    h.bodyLen = 12345;
    h.seq = 0xdeadbeefcafe;
    uint8_t buf[kHeadSize];
    packHead(h, buf);
    Head g;
    TEST_CHECK(unpackHead(buf, g));
    TEST_CHECK_EQ(g.kind, (uint16_t)Kind::SNAP);
    TEST_CHECK_EQ(g.bodyLen, 12345);
    TEST_CHECK(g.seq == 0xdeadbeefcafeull);

    // 坏 magic
    uint8_t b2[kHeadSize]; memcpy(b2, buf, kHeadSize);
    b2[0] ^= 0xff;
    Head bad;
    TEST_CHECK_MSG(!unpackHead(b2, bad), "坏 magic 应拒");
    TEST_CHECK(headErr(b2, bad) == FrameErr::BAD_MAGIC);

    // 坏版本
    uint8_t b3[kHeadSize]; packHead(h, b3);
    b3[5] = 99;   // version 低字节
    TEST_CHECK_MSG(!unpackHead(b3, bad), "坏版本应拒");
    TEST_CHECK(headErr(b3, bad) == FrameErr::BAD_VER);

    // body 超限
    Head big; big.kind = (uint16_t)Kind::PING; big.bodyLen = kMaxBody + 1;
    uint8_t b4[kHeadSize]; packHead(big, b4);
    TEST_CHECK_MSG(!unpackHead(b4, bad), "body 超限应拒");
    TEST_CHECK(headErr(b4, bad) == FrameErr::TOO_BIG);

    Head badKind; badKind.kind = 999;
    uint8_t b5[kHeadSize]; packHead(badKind, b5);
    TEST_CHECK_MSG(!unpackHead(b5, bad), "未知消息应拒");
    TEST_CHECK(headErr(b5, bad) == FrameErr::BAD_KIND);

    ProtoErr err;
    TEST_CHECK(!sendMsg(nullptr, static_cast<Kind>(999), "", 0, &err));
    TEST_CHECK(err.frame == FrameErr::BAD_KIND);
}

static void testWire() {
    // Risk 往返
    Risk r;
    r.id = "e1"; parseCidr("1.2.3.4", r.ip);
    r.src = Src::WAF; r.type = RiskType::INJECT;
    r.severity = 7; r.banMs = 60000; r.atMs = 123456; r.reason = "sqli";
    Risk r2;
    TEST_CHECK(riskFromJson(riskToJson(r), r2));
    TEST_CHECK_EQ(r2.id, std::string("e1"));
    TEST_CHECK(r2.ip == r.ip);
    TEST_CHECK(r2.src == Src::WAF);
    TEST_CHECK_EQ(r2.severity, 7);
    Rule badRule;
    TEST_CHECK_MSG(!ruleFromJson("{\"id\":\"\",\"ip\":\"1.2.3.4\"}", badRule),
                   "empty rule id must be rejected");
    TEST_CHECK_MSG(!riskFromJson("{\"ip\":\"1.2.3.4\",\"src\":\"emerg\"}", r2),
                   "report must not claim a rule-only source");
    TEST_CHECK_MSG(!riskFromJson("{\"ip\":\"1.2.3.4\",\"src\":\"unknown\"}", r2),
                   "unknown report source must be rejected");

    // Rule 往返
    Rule ru;
    ru.id = "r1"; parseCidr("10.0.0.0/8", ru.ip);
    ru.action = Act::DENY; ru.src = Src::RATE;
    ru.priority = rulePriority(Src::RATE, Act::DENY);
    ru.createdAtMs = 111; ru.expireAtMs = 222; ru.version = 5; ru.reason = "abuse";
    Rule ru2;
    TEST_CHECK(ruleFromJson(ruleToJson(ru), ru2));
    TEST_CHECK_EQ(ru2.id, std::string("r1"));
    TEST_CHECK(ru2.ip == ru.ip);
    TEST_CHECK(ru2.action == Act::DENY);
    TEST_CHECK_EQ(ru2.priority, 400);
    TEST_CHECK(ru2.version == 5u);
    TEST_CHECK_MSG(!ruleFromJson("{\"id\":\"r1\",\"ip\":\"1.2.3.4\",\"action\":\"bad\",\"src\":\"rate\",\"priority\":400}", ru2),
                   "未知 action 应拒");
    TEST_CHECK_MSG(!ruleFromJson("{\"id\":\"r1\",\"ip\":\"1.2.3.4\",\"action\":\"deny\",\"src\":\"bad\",\"priority\":400}", ru2),
                   "未知 src 应拒");
    TEST_CHECK_MSG(!ruleFromJson("{\"id\":\"r1\",\"ip\":\"1.2.3.4\",\"action\":\"allow\",\"src\":\"emerg\",\"priority\":1000}", ru2),
                   "emerg allow 应拒");
    TEST_CHECK_MSG(!ruleFromJson("{\"id\":\"r1\",\"ip\":\"1.2.3.4\",\"action\":\"deny\",\"src\":\"rate\",\"priority\":400}", ru2),
                   "规则缺时间和版本应拒");
    TEST_CHECK(ruleFromJson("{\"id\":\"r1\",\"ip\":\"1.2.3.4\",\"action\":\"deny\",\"src\":\"rate\",\"priority\":999999,\"created_ms\":1,\"expire_ms\":0,\"version\":1,\"reason\":\"test\"}", ru2));
    TEST_CHECK_EQ(ru2.priority, rulePriority(Src::RATE, Act::DENY));

    // Snap 往返
    std::vector<Rule> rules = {ru, ru};
    rules[1].id = "r2";
    uint64_t ep = 0, ver = 0;
    std::vector<Rule> got;
    TEST_CHECK(snapFromJson(snapToJson(0xABCD, 100, rules), ep, ver, got));
    TEST_CHECK(ep == 0xABCDu);
    TEST_CHECK(ver == 100u);
    TEST_CHECK_EQ(got.size(), 2u);
    TEST_CHECK_EQ(got[1].id, std::string("r2"));
    rules[1].id = ru.id;
    TEST_CHECK_MSG(!snapFromJson(snapToJson(0xABCD, 100, rules), ep, ver, got),
                   "重复 rule id 应拒");

    // Hello 往返
    std::string inst; uint64_t he = 0, hv = 0;
    TEST_CHECK(helloFromJson(helloToJson("gw-1", 77, 42), inst, he, hv));
    TEST_CHECK_EQ(inst, std::string("gw-1"));
    TEST_CHECK(he == 77u);
    TEST_CHECK(hv == 42u);

    TEST_CHECK(readyFromJson(readyToJson(77, 42), he, hv));
    TEST_CHECK(he == 77u && hv == 42u);
    std::string code;
    TEST_CHECK(errFromJson(errToJson("bad_ack"), code));
    TEST_CHECK_EQ(code, std::string("bad_ack"));

    std::vector<DeltaOp> ops;
    TEST_CHECK_MSG(!deltaFromJson("{\"epoch\":1,\"prev\":1,\"ver\":2,\"ops\":[{\"del\":true}]}",
                                  ep, ver, hv, ops),
                   "delete delta without rule id must be rejected");
    TEST_CHECK_MSG(!snapFromJson("{\"epoch\":1,\"version\":1,\"rules\":[{\"id\":\"\",\"ip\":\"1.2.3.4\"}]}",
                                 ep, ver, rules),
                   "snapshot rule without id must be rejected");

    // 坏 JSON
    Rule junk;
    TEST_CHECK(!ruleFromJson("{not json", junk));
    TEST_CHECK(!ruleFromJson("{\"ip\":\"bad\"}", junk));
}

static Ip cidr(const char* s) { Ip ip; parseCidr(s, ip); return ip; }

static void testResolver() {
    std::vector<Ip> trusted = { cidr("10.0.0.0/8"), cidr("192.168.1.1") };

    // peer 不是可信代理: 直连, XFF 一概不信(防伪造)
    Ip r = resolveClientAddr(cidr("8.8.8.8"), "1.2.3.4", trusted);
    TEST_CHECK_MSG(r == cidr("8.8.8.8"), "直连者伪造 XFF 应无效, 用 peer");

    // peer 是可信代理, XFF 一跳: 取真实客户端
    r = resolveClientAddr(cidr("10.1.2.3"), "1.2.3.4", trusted);
    TEST_CHECK_MSG(r == cidr("1.2.3.4"), "可信代理后应取 XFF 客户端");

    // peer 可信, XFF 多跳(右边是内层可信代理): 从右往左跳过可信, 第一个非可信
    r = resolveClientAddr(cidr("10.1.2.3"), "1.2.3.4, 10.9.9.9", trusted);
    TEST_CHECK_MSG(r == cidr("1.2.3.4"), "应跳过右侧可信跳取真实客户端");

    // peer 可信但 XFF 空: 退回 peer
    r = resolveClientAddr(cidr("10.1.2.3"), "", trusted);
    TEST_CHECK_MSG(r == cidr("10.1.2.3"), "XFF 空应退回 peer");

    // peer 可信, XFF 全是可信代理: 退回 peer
    r = resolveClientAddr(cidr("10.1.2.3"), "10.1.1.1, 192.168.1.1", trusted);
    TEST_CHECK_MSG(r == cidr("10.1.2.3"), "XFF 全可信应退回 peer");

    Ip parsed;
    TEST_CHECK(!resolveClientAddr(cidr("10.1.2.3"), " 5.6.7.8 , garbage ", trusted, parsed));
    TEST_CHECK(!resolveClientAddr(cidr("10.1.2.3"), "198.51.100.4/24", trusted, parsed));
    TEST_CHECK(!resolveClientAddr(cidr("10.1.2.3"), "2001:db8::1/64", trusted, parsed));
    TEST_CHECK(!resolveClientAddr(cidr("10.1.2.3"), ", 10.1.1.1", trusted, parsed));

    std::string longXff;
    for(int i = 0; i < 700; ++i) {
        if(!longXff.empty()) longXff += ',';
        longXff += "10.1.1.1";
    }
    r = resolveClientAddr(cidr("10.1.2.3"), longXff, trusted);
    TEST_CHECK(r == cidr("10.1.2.3"));

    // 空可信名单: 恒用 peer(没配代理的默认)
    r = resolveClientAddr(cidr("8.8.8.8"), "1.2.3.4", {});
    TEST_CHECK_MSG(r == cidr("8.8.8.8"), "空名单只信 peer");

    // isTrustedProxy 基本
    TEST_CHECK(isTrustedProxy(cidr("10.255.0.1"), trusted));
    TEST_CHECK(!isTrustedProxy(cidr("11.0.0.1"), trusted));
}

static void testPriority() {
    // 优先级压制关系
    TEST_CHECK(rulePriority(Src::EMERG, Act::DENY) > rulePriority(Src::ADMIN, Act::DENY));
    TEST_CHECK(rulePriority(Src::ADMIN, Act::DENY) > rulePriority(Src::ADMIN, Act::ALLOW));
    TEST_CHECK(rulePriority(Src::STATIC, Act::ALLOW) > rulePriority(Src::WAF, Act::DENY));
    TEST_CHECK(rulePriority(Src::WAF, Act::DENY) > rulePriority(Src::RATE, Act::DENY));
    TEST_CHECK(rulePriority(Src::RATE, Act::DENY) > rulePriority(Src::STATIC, Act::DENY));
}

int main() {
    testIp();
    testCidr();
    testHead();
    testWire();
    testResolver();
    testPriority();
    return TEST_SUMMARY();
}
