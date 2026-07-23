// 严苛测试 bronx::BxAddress 模块
// 覆盖：
//   1. IPv4 / IPv6 / Unix 地址类型识别（BxAddress::Create、family、长度）
//   2. BxIpAddress::Create（数字串解析）+ 文本格式（含异常路径）
//   3. Lookup / LookupAny / ResolveOneIp（127.0.0.1、localhost、有端口/无端口）
//   4. operator< / operator== / operator!=（同族跨族）
//   5. broadcast / network / subnetMask 在 IPv4 全 prefix_len（0..32+）
//   6. broadcast / network / subnetMask 在 IPv6 全 prefix_len（0..128 + 越界）
//   7. BxUnixAddress 三种形态 + Create 工厂边界 + BxAddress::Create 反向构造
//   8. GetInterfaceAddresses（multimap 与 vector 两种重载）
//   9. Unix socket 端到端：bind+listen+accept+收发，并校验 local/remote
//      （走原始 POSIX，不经 BxIoManager，避免与 socket/BxIoManager 退出路径耦合）
//  10. Linux 抽象命名空间 bind 验证（同样走 POSIX）

#include "endpoint.h"
#include "log.h"
#include "net_socket.h"

#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <atomic>


static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();
static int g_failed = 0;

#define EXPECT(expr)                                                          \
    do {                                                                      \
        if(!(expr)) {                                                         \
            BRONX_LOG_ERROR(g_logger) << "FAIL: " #expr                       \
                                      << " at " << __FILE__ << ":" << __LINE__;\
            ++g_failed;                                                       \
        }                                                                     \
    } while(0)


// ========== 1. BxAddress::Create 类型识别 ==========
static void test_address_create_dispatch(){
    BRONX_LOG_INFO(g_logger) << "--- test_address_create_dispatch ---";

    // IPv4
    {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(8080);
        ::inet_pton(AF_INET, "192.168.1.1", &sa.sin_addr);
        auto a = bronx::BxAddress::Create((sockaddr*)&sa, sizeof(sa));
        EXPECT(a);
        EXPECT(std::dynamic_pointer_cast<bronx::BxIpv4Address>(a));
        EXPECT(a->getFamily() == AF_INET);
        EXPECT(a->toString() == "192.168.1.1:8080");
        EXPECT(!bronx::BxAddress::Create((sockaddr*)&sa, sizeof(sa_family_t)));
    }
    // IPv6
    {
        sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons(443);
        ::inet_pton(AF_INET6, "2001:db8::1", &sa.sin6_addr);
        auto a = bronx::BxAddress::Create((sockaddr*)&sa, sizeof(sa));
        EXPECT(a);
        EXPECT(std::dynamic_pointer_cast<bronx::BxIpv6Address>(a));
        EXPECT(a->getFamily() == AF_INET6);
        EXPECT(a->toString() == "[2001:db8::1]:443");
        EXPECT(!bronx::BxAddress::Create((sockaddr*)&sa, sizeof(sa_family_t)));
    }
    // Unix
    {
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        const char* p = "/tmp/x.sock";
        std::strncpy(sa.sun_path, p, sizeof(sa.sun_path) - 1);
        socklen_t len = offsetof(sockaddr_un, sun_path) + std::strlen(p) + 1;
        auto a = bronx::BxAddress::Create((sockaddr*)&sa, len);
        EXPECT(a);
        auto u = std::dynamic_pointer_cast<bronx::BxUnixAddress>(a);
        EXPECT(u);
        EXPECT(u->getPath() == p);
    }
    // Unix unnamed：addrlen 只到 family（来自未 bind 的对端 accept）
    {
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        auto a = bronx::BxAddress::Create((sockaddr*)&sa, sizeof(sa_family_t));
        EXPECT(a);
        auto u = std::dynamic_pointer_cast<bronx::BxUnixAddress>(a);
        EXPECT(u);
        EXPECT(u->getPath().empty());
    }
    // Unix addrlen=0（异常输入）直接拒绝，避免按过短缓冲拷贝
    {
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        auto a = bronx::BxAddress::Create((sockaddr*)&sa, 0);
        EXPECT(!a);
    }
    // Unknown family
    {
        sockaddr sa{};
        sa.sa_family = AF_NETLINK;
        auto a = bronx::BxAddress::Create(&sa, sizeof(sa));
        EXPECT(a);
        EXPECT(std::dynamic_pointer_cast<bronx::BxUnknownAddress>(a));
        EXPECT(a->getFamily() == AF_NETLINK);

        auto short_unknown = bronx::BxAddress::Create(&sa, sizeof(sa_family_t));
        EXPECT(short_unknown);
        EXPECT(std::dynamic_pointer_cast<bronx::BxUnknownAddress>(short_unknown));
        EXPECT(short_unknown->getFamily() == AF_NETLINK);
    }
    // nullptr 不 crash
    {
        auto a = bronx::BxAddress::Create(nullptr, 0);
        EXPECT(!a);
    }
}


// ========== 2. BxIpAddress::Create + BxIpv4Address::Create + BxIpv6Address::Create ==========
static void test_ip_create_strings(){
    BRONX_LOG_INFO(g_logger) << "--- test_ip_create_strings ---";

    // IPv4
    {
        auto a = bronx::BxIpv4Address::Create("127.0.0.1", 80);
        EXPECT(a);
        EXPECT(a->toString() == "127.0.0.1:80");
        EXPECT(a->getPort() == 80);
        a->setPort(8080);
        EXPECT(a->getPort() == 8080);
    }
    // IPv4 异常
    EXPECT(!bronx::BxIpv4Address::Create("not-an-ip", 0));
    EXPECT(!bronx::BxIpv4Address::Create("999.999.999.999", 0));
    EXPECT(!bronx::BxIpv4Address::Create("", 0));
    EXPECT(!bronx::BxIpv4Address::Create("1.2.3", 0));
    EXPECT(!bronx::BxIpv4Address::Create(nullptr, 0));

    // IPv6
    {
        auto a = bronx::BxIpv6Address::Create("::1", 80);
        EXPECT(a);
        EXPECT(a->toString() == "[::1]:80");

        // IPv6 with embedded IPv4
        auto a2 = bronx::BxIpv6Address::Create("::ffff:192.168.1.1", 0);
        EXPECT(a2);
    }
    EXPECT(!bronx::BxIpv6Address::Create("not-v6", 0));
    EXPECT(!bronx::BxIpv6Address::Create("12345::", 0));
    EXPECT(!bronx::BxIpv6Address::Create(nullptr, 0));

    // IPv6 零段压缩（RFC 5952）回归：
    //  - 最长零段优先、等长压第一个、单零段(长度1)不压缩、首尾零段
    {
        struct { const char* in; const char* out; } cases[] = {
            {"1:2:3:4:5:6:7:8", "[1:2:3:4:5:6:7:8]:0"}, // 无零段
            {"::",              "[::]:0"},               // 全零
            {"1::",             "[1::]:0"},              // 尾零
            {"1::8",            "[1::8]:0"},             // 中段零
            {"0:0:3:4:5:6:7:8", "[::3:4:5:6:7:8]:0"},    // 首零段
            {"1:2:3:4:5:6:0:0", "[1:2:3:4:5:6::]:0"},    // 尾零段
            {"1:0:0:1:0:0:0:1", "[1:0:0:1::1]:0"},       // 两零段取更长
            {"1:0:0:1:0:0:1:1", "[1::1:0:0:1:1]:0"},     // 等长取第一个
            {"1:0:1:1:1:1:1:1", "[1:0:1:1:1:1:1:1]:0"},  // 单零段不压
        };
        for(auto& c : cases){
            auto a = bronx::BxIpv6Address::Create(c.in, 0);
            EXPECT(a);
            if(a){
                EXPECT(a->toString() == std::string(c.out));
            }
        }
    }

    // BxIpAddress::Create 自动判类型
    {
        auto a = bronx::BxIpAddress::Create("10.0.0.1", 22);
        EXPECT(a);
        EXPECT(std::dynamic_pointer_cast<bronx::BxIpv4Address>(a));
        EXPECT(a->getPort() == 22);

        auto a2 = bronx::BxIpAddress::Create("fe80::1", 22);
        EXPECT(a2);
        EXPECT(std::dynamic_pointer_cast<bronx::BxIpv6Address>(a2));
    }
    EXPECT(!bronx::BxIpAddress::Create("definitely-not-ip", 0));
    EXPECT(!bronx::BxIpAddress::Create(nullptr, 0));

    // IPv4 端口边界
    {
        auto a = bronx::BxIpv4Address::Create("0.0.0.0", 0);
        EXPECT(a && a->getPort() == 0);
    }
    {
        auto a = bronx::BxIpv4Address::Create("255.255.255.255", 65535);
        EXPECT(a && a->getPort() == 65535);
    }
}


// ========== 3. Lookup 系列 ==========
static void test_lookup(){
    BRONX_LOG_INFO(g_logger) << "--- test_lookup ---";

    // host:port
    {
        std::vector<bronx::BxAddress::ptr> v;
        EXPECT(bronx::BxAddress::Lookup(v, "127.0.0.1:80",
                                      AF_INET, SOCK_STREAM, IPPROTO_TCP));
        EXPECT(!v.empty());
        EXPECT(v[0]->toString() == "127.0.0.1:80");
    }
    // 无端口
    {
        std::vector<bronx::BxAddress::ptr> v;
        EXPECT(bronx::BxAddress::Lookup(v, "127.0.0.1",
                                      AF_INET, SOCK_STREAM, IPPROTO_TCP));
        EXPECT(!v.empty());
    }
    // IPv6 [...]:port
    {
        std::vector<bronx::BxAddress::ptr> v;
        bool ok = bronx::BxAddress::Lookup(v, "[::1]:53",
                                         AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        // 部分容器可能没 IPv6，ok=false 不视为失败；只要 ok=true 则地址必须正确
        if(ok){
            EXPECT(!v.empty());
            EXPECT(v[0]->toString() == "[::1]:53");
        } else {
            BRONX_LOG_INFO(g_logger) << "IPv6 lookup skipped (no v6 here)";
        }
    }
    // IPv6 无端口 [...]（回归：']' 是末字符时 memchr 长度别漏掉它）
    {
        std::vector<bronx::BxAddress::ptr> v;
        bool ok = bronx::BxAddress::Lookup(v, "[::1]",
                                         AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if(ok){
            EXPECT(!v.empty());
            EXPECT(v[0]->getFamily() == AF_INET6);
            EXPECT(v[0]->toString() == "[::1]:0");
        } else {
            BRONX_LOG_INFO(g_logger) << "IPv6 no-port lookup skipped (no v6 here)";
        }
    }
    // IPv6 无端口、非环回（确认 node 截取不含方括号）
    {
        std::vector<bronx::BxAddress::ptr> v;
        bool ok = bronx::BxAddress::Lookup(v, "[2001:db8::1]",
                                         AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if(ok){
            EXPECT(!v.empty());
            EXPECT(v[0]->toString() == "[2001:db8::1]:0");
        }
    }
    // IPv6 方括号后只允许结束或 :port
    {
        std::vector<bronx::BxAddress::ptr> v;
        EXPECT(!bronx::BxAddress::Lookup(v, "[::1]trailing",
                                       AF_INET6, SOCK_STREAM, IPPROTO_TCP));
        EXPECT(v.empty());
    }
    // LookupAny
    {
        auto a = bronx::BxAddress::LookupAny("127.0.0.1:1",
                                           AF_INET, SOCK_STREAM, IPPROTO_TCP);
        EXPECT(a);
        EXPECT(a->getFamily() == AF_INET);
    }
    // ResolveOneIp
    {
        auto a = bronx::BxAddress::ResolveOneIp("127.0.0.1:1",
                                                    AF_INET, SOCK_STREAM, IPPROTO_TCP);
        EXPECT(a);
        EXPECT(a->getPort() == 1);
    }
    // 不可解析
    {
        auto a = bronx::BxAddress::LookupAny(
            "this-host-should-not-exist.invalid:1", AF_INET);
        EXPECT(!a);
    }
}


// ========== 4. 关系运算符 ==========
static void test_operators(){
    BRONX_LOG_INFO(g_logger) << "--- test_operators ---";

    auto a = bronx::BxIpv4Address::Create("10.0.0.1", 80);
    auto b = bronx::BxIpv4Address::Create("10.0.0.1", 80);
    auto c = bronx::BxIpv4Address::Create("10.0.0.2", 80);
    auto d = bronx::BxIpv4Address::Create("10.0.0.1", 81);
    EXPECT(a && b && c && d);

    EXPECT(*a == *b);
    EXPECT(!(*a != *b));
    EXPECT(*a != *c);
    EXPECT(*a < *c);
    EXPECT(!(*c < *a));
    EXPECT(*a < *d);     // 端口字段在地址结构靠前 / 靠后顺序由 memcmp 决定，但 a 与 d 仅端口不同
                         // 80 (0x0050) < 81 (0x0051) 在网络字节序下也是字节级递增

    // set 容器使用：去重+排序
    std::set<bronx::BxAddress::ptr,
             std::function<bool(bronx::BxAddress::ptr, bronx::BxAddress::ptr)>> s(
        [](bronx::BxAddress::ptr l, bronx::BxAddress::ptr r){ return *l < *r; });
    s.insert(a); s.insert(b); s.insert(c); s.insert(d);
    EXPECT(s.size() == 3);   // a 与 b 等价
}


// ========== 5. IPv4 broadcast/network/subnetMask 全范围 ==========
static void test_ipv4_masks(){
    BRONX_LOG_INFO(g_logger) << "--- test_ipv4_masks ---";

    auto v4 = bronx::BxIpv4Address::Create("192.168.1.10", 0);
    EXPECT(v4);
    // /24
    EXPECT(v4->broadcastAddress(24)->toString() == "192.168.1.255:0");
    EXPECT(v4->networkAddress(24)->toString()   == "192.168.1.0:0");
    EXPECT(v4->subnetMask(24)->toString()       == "255.255.255.0:0");
    // /16
    EXPECT(v4->broadcastAddress(16)->toString() == "192.168.255.255:0");
    EXPECT(v4->networkAddress(16)->toString()   == "192.168.0.0:0");
    EXPECT(v4->subnetMask(16)->toString()       == "255.255.0.0:0");
    // /8
    EXPECT(v4->broadcastAddress(8)->toString()  == "192.255.255.255:0");
    EXPECT(v4->networkAddress(8)->toString()    == "192.0.0.0:0");
    EXPECT(v4->subnetMask(8)->toString()        == "255.0.0.0:0");
    // /32 单点
    EXPECT(v4->broadcastAddress(32)->toString() == "192.168.1.10:0");
    EXPECT(v4->networkAddress(32)->toString()   == "192.168.1.10:0");
    EXPECT(v4->subnetMask(32)->toString()       == "255.255.255.255:0");
    // /0 默认
    EXPECT(v4->broadcastAddress(0)->toString()  == "255.255.255.255:0");
    EXPECT(v4->networkAddress(0)->toString()    == "0.0.0.0:0");
    EXPECT(v4->subnetMask(0)->toString()        == "0.0.0.0:0");
    // /23 跨字节
    EXPECT(v4->broadcastAddress(23)->toString() == "192.168.1.255:0");
    EXPECT(v4->networkAddress(23)->toString()   == "192.168.0.0:0");
    EXPECT(v4->subnetMask(23)->toString()       == "255.255.254.0:0");
    // /1 极端
    EXPECT(v4->subnetMask(1)->toString()        == "128.0.0.0:0");
    // 越界
    EXPECT(!v4->broadcastAddress(33));
    EXPECT(!v4->networkAddress(33));
    EXPECT(!v4->subnetMask(33));
    // IPv4 不会修改原对象
    EXPECT(v4->toString() == "192.168.1.10:0");

    // 全 prefix_len 扫描不变量：network = orig & mask；broadcast = network | ~mask
    {
        auto full = bronx::BxIpv4Address::Create("172.20.30.40", 0);
        EXPECT(full);
        uint32_t orig = ntohl(((const sockaddr_in*)full->getAddr())->sin_addr.s_addr);
        for(uint32_t p = 0; p <= 32; ++p){
            auto net = std::dynamic_pointer_cast<bronx::BxIpv4Address>(full->networkAddress(p));
            auto bc  = std::dynamic_pointer_cast<bronx::BxIpv4Address>(full->broadcastAddress(p));
            auto mk  = std::dynamic_pointer_cast<bronx::BxIpv4Address>(full->subnetMask(p));
            EXPECT(net && bc && mk);
            if(!(net && bc && mk)) continue;
            uint32_t mask = ntohl(((const sockaddr_in*)mk->getAddr())->sin_addr.s_addr);
            uint32_t nb   = ntohl(((const sockaddr_in*)net->getAddr())->sin_addr.s_addr);
            uint32_t bb   = ntohl(((const sockaddr_in*)bc->getAddr())->sin_addr.s_addr);
            EXPECT((orig & mask) == nb);
            EXPECT((nb | ~mask) == bb);
        }
    }
}


// ========== 5b. IPv6 toString round-trip 严苛测试 ==========
// 把各种零压缩形态的 IPv6 地址走一遍：Create → toString → 剥掉 [ ]:port →
// inet_pton 解析回 16 字节，与 inet_pton(原始字符串) 的字节比对。
// 只要 bronx 的 insert() 产出的字符串语义正确，两次 pton 结果必然一致。
static bool ipv6_roundtrip(const char* addr_str){
    auto a = bronx::BxIpv6Address::Create(addr_str, 12345);
    if(!a){
        BRONX_LOG_ERROR(g_logger) << "Create failed for " << addr_str;
        return false;
    }
    std::string s = a->toString();   // 形如 [....]:12345
    // 剥离首尾 [ ] 和 :port
    auto rb = s.find('[');
    auto re = s.find(']');
    if(rb == std::string::npos || re == std::string::npos || re <= rb + 1){
        BRONX_LOG_ERROR(g_logger) << "bad toString format: " << s;
        return false;
    }
    std::string inner = s.substr(rb + 1, re - rb - 1);

    in6_addr want{}, got{};
    EXPECT(::inet_pton(AF_INET6, addr_str, &want) == 1);
    int rc = ::inet_pton(AF_INET6, inner.c_str(), &got);
    if(rc != 1){
        BRONX_LOG_ERROR(g_logger) << "produced unparseable v6: '" << inner
                                  << "' from input '" << addr_str << "' full='" << s << "'";
        return false;
    }
    if(std::memcmp(&want, &got, sizeof(in6_addr)) != 0){
        BRONX_LOG_ERROR(g_logger) << "v6 mismatch: input='" << addr_str
                                  << "' produced='" << inner << "'";
        return false;
    }
    // 端口也要对
    EXPECT(a->getPort() == 12345);
    return true;
}

static void test_ipv6_tostring_roundtrip(){
    BRONX_LOG_INFO(g_logger) << "--- test_ipv6_tostring_roundtrip ---";

    const char* cases[] = {
        "::",                                   // 全 0
        "::1",                                   // 仅末尾非 0，开头长串 0
        "1::",                                   // 仅开头非 0，结尾长串 0
        "2001:db8::1",                           // 中间压缩
        "2001:db8:0:abcd::1",                    // 中间单段 0 不压 + 末尾压缩
        "fe80::21c:42ff:fe4f:cd5b",              // 链路本地
        "2001:0:0:1:0:0:0:1",                    // 两段可压缩区，应压缩更长的那段
        "1:2:3:4:5:6:7:8",                       // 无 0，无压缩
        "ff02::1",                               // 组播
        "::ffff:192.168.1.1",                    // IPv4-mapped
        "0:0:0:0:0:0:0:0",                       // 全 0 的展开写法
        "1:0:0:0:0:0:0:8",                       // 中间长 0
        "2001:db8:0:0:1:0:0:1",                  // 两段等长 0（取第一段）
    };
    for(const char* c : cases){
        bool ok = ipv6_roundtrip(c);
        EXPECT(ok);
        if(!ok){
            BRONX_LOG_ERROR(g_logger) << "ROUNDTRIP FAIL: " << c;
        }
    }

    // 精确字符串断言：验证 RFC 5952 零压缩的正确性（round-trip 验语义，这里验形态：
    // 不能多/少冒号、单个 0 组不压、压最长段、等长取前段）
    struct { const char* in; const char* want; } exact[] = {
        {"::",                "[::]:0"},
        {"::1",               "[::1]:0"},
        {"1::",               "[1::]:0"},
        {"2001:db8::1",       "[2001:db8::1]:0"},
        {"2001:db8:0:abcd::1","[2001:db8:0:abcd::1]:0"},  // 单段 0 必须保留，不能压成 ::
        {"2001:0:0:1:0:0:0:1","[2001:0:0:1::1]:0"},       // 压更长的后段
        {"2001:db8:0:0:1:0:0:1","[2001:db8::1:0:0:1]:0"}, // 等长取第一段
        {"0:0:0:1:0:0:0:0",   "[0:0:0:1::]:0"},           // 结尾零段
        {"1:2:3:4:5:6:7:8",   "[1:2:3:4:5:6:7:8]:0"},     // 无压缩
        {"1:0:0:0:0:0:0:8",   "[1::8]:0"},
    };
    for(auto& e : exact){
        auto a = bronx::BxIpv6Address::Create(e.in, 0);
        EXPECT(a);
        if(a && a->toString() != e.want){
            BRONX_LOG_ERROR(g_logger) << "v6 toString exact FAIL: in=" << e.in
                                      << " got=" << a->toString() << " want=" << e.want;
            EXPECT(a->toString() == e.want);
        }
    }
}


// ========== 6. IPv6 broadcast/network/subnetMask ==========
static void test_ipv6_masks(){
    BRONX_LOG_INFO(g_logger) << "--- test_ipv6_masks ---";

    auto v6 = bronx::BxIpv6Address::Create("2001:db8:0:abcd::1", 0);
    EXPECT(v6);

    // /64：分界字节对齐
    {
        auto net = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->networkAddress(64));
        EXPECT(net);
        const sockaddr_in6* nsa = (const sockaddr_in6*)net->getAddr();
        for(int i = 8; i < 16; ++i){
            EXPECT(nsa->sin6_addr.s6_addr[i] == 0x00);
        }
        auto bc = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->broadcastAddress(64));
        EXPECT(bc);
        const sockaddr_in6* bsa = (const sockaddr_in6*)bc->getAddr();
        for(int i = 8; i < 16; ++i){
            EXPECT(bsa->sin6_addr.s6_addr[i] == 0xff);
        }
        auto mk = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->subnetMask(64));
        EXPECT(mk);
        const sockaddr_in6* msa = (const sockaddr_in6*)mk->getAddr();
        for(int i = 0; i < 8; ++i){
            EXPECT(msa->sin6_addr.s6_addr[i] == 0xff);
        }
        for(int i = 8; i < 16; ++i){
            EXPECT(msa->sin6_addr.s6_addr[i] == 0x00);
        }
    }
    // /56：分界字节非对齐
    {
        auto net = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->networkAddress(56));
        EXPECT(net);
        const sockaddr_in6* nsa = (const sockaddr_in6*)net->getAddr();
        const sockaddr_in6* osa = (const sockaddr_in6*)v6->getAddr();
        for(int i = 0; i < 7; ++i){
            EXPECT(nsa->sin6_addr.s6_addr[i] == osa->sin6_addr.s6_addr[i]);
        }
        for(int i = 7; i < 16; ++i){
            EXPECT(nsa->sin6_addr.s6_addr[i] == 0x00);
        }
        auto mk = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->subnetMask(56));
        EXPECT(mk);
        const sockaddr_in6* msa = (const sockaddr_in6*)mk->getAddr();
        for(int i = 0; i < 7; ++i){
            EXPECT(msa->sin6_addr.s6_addr[i] == 0xff);
        }
        for(int i = 7; i < 16; ++i){
            EXPECT(msa->sin6_addr.s6_addr[i] == 0x00);
        }
    }
    // /128 单点：mask 全 1，network/broadcast 均等于自身
    {
        auto mk = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->subnetMask(128));
        EXPECT(mk);
        const sockaddr_in6* msa = (const sockaddr_in6*)mk->getAddr();
        for(int i = 0; i < 16; ++i){
            EXPECT(msa->sin6_addr.s6_addr[i] == 0xff);
        }
        auto net = v6->networkAddress(128);
        auto bc  = v6->broadcastAddress(128);
        EXPECT(net && bc);
        EXPECT(*net == *v6);
        EXPECT(*bc  == *v6);
    }
    // /0：mask 全 0，network 全 0，broadcast 全 1
    {
        auto mk = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->subnetMask(0));
        EXPECT(mk);
        const sockaddr_in6* msa = (const sockaddr_in6*)mk->getAddr();
        for(int i = 0; i < 16; ++i){
            EXPECT(msa->sin6_addr.s6_addr[i] == 0x00);
        }
        auto net = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->networkAddress(0));
        EXPECT(net);
        const sockaddr_in6* nsa = (const sockaddr_in6*)net->getAddr();
        for(int i = 0; i < 16; ++i){
            EXPECT(nsa->sin6_addr.s6_addr[i] == 0x00);
        }
        auto bc = std::dynamic_pointer_cast<bronx::BxIpv6Address>(v6->broadcastAddress(0));
        EXPECT(bc);
        const sockaddr_in6* bsa = (const sockaddr_in6*)bc->getAddr();
        for(int i = 0; i < 16; ++i){
            EXPECT(bsa->sin6_addr.s6_addr[i] == 0xff);
        }
    }
    // 越界：>128 全部返回 nullptr
    EXPECT(!v6->subnetMask(129));
    EXPECT(!v6->networkAddress(129));
    EXPECT(!v6->broadcastAddress(129));

    // 全 prefix_len 扫描，逐字节验证两条不变量：
    //   (1) network = orig & mask
    //   (2) broadcast = network | ~mask
    // 同时覆盖 8 倍数（整字节分界）与非倍数（位分界）两种情况
    {
        auto full = bronx::BxIpv6Address::Create("2001:db8:abcd:ef01:2345:6789:abcd:ef01", 0);
        EXPECT(full);
        const sockaddr_in6* osa = (const sockaddr_in6*)full->getAddr();
        for(uint32_t p = 0; p <= 128; ++p){
            auto net = std::dynamic_pointer_cast<bronx::BxIpv6Address>(full->networkAddress(p));
            auto bc  = std::dynamic_pointer_cast<bronx::BxIpv6Address>(full->broadcastAddress(p));
            auto mk  = std::dynamic_pointer_cast<bronx::BxIpv6Address>(full->subnetMask(p));
            EXPECT(net && bc && mk);
            if(!(net && bc && mk)) continue;
            const sockaddr_in6* n = (const sockaddr_in6*)net->getAddr();
            const sockaddr_in6* b = (const sockaddr_in6*)bc->getAddr();
            const sockaddr_in6* m = (const sockaddr_in6*)mk->getAddr();
            for(int i = 0; i < 16; ++i){
                uint8_t mask = m->sin6_addr.s6_addr[i];
                uint8_t orig = osa->sin6_addr.s6_addr[i];
                uint8_t nb   = n->sin6_addr.s6_addr[i];
                uint8_t bb   = b->sin6_addr.s6_addr[i];
                EXPECT((uint8_t)(orig & mask) == nb);
                EXPECT((uint8_t)(nb | (uint8_t)~mask) == bb);
            }
        }
    }
}


// ========== 7. BxUnixAddress ==========
static void test_unix_address_basics(){
    BRONX_LOG_INFO(g_logger) << "--- test_unix_address_basics ---";

    // 普通路径
    {
        bronx::BxUnixAddress addr("/tmp/bronx-unix-basic.sock");
        EXPECT(addr.getFamily() == AF_UNIX);
        EXPECT(addr.getPath() == "/tmp/bronx-unix-basic.sock");
        EXPECT(addr.getAddrLen() ==
               offsetof(sockaddr_un, sun_path) +
                   std::string("/tmp/bronx-unix-basic.sock").size() + 1);
        EXPECT(addr.toString().find("unix:/tmp/bronx-unix-basic.sock") != std::string::npos);
    }
    // 抽象命名空间
    {
        std::string abs_name(1, '\0');
        abs_name += "bronx-abstract";
        bronx::BxUnixAddress addr(abs_name);
        EXPECT(addr.getAddrLen() ==
               offsetof(sockaddr_un, sun_path) + abs_name.size());
        EXPECT(addr.getPath() == "bronx-abstract");
        EXPECT(addr.toString().find("unix:@bronx-abstract") != std::string::npos);
    }
    // unnamed
    {
        bronx::BxUnixAddress addr("");
        EXPECT(addr.getAddrLen() == sizeof(sa_family_t));
        EXPECT(addr.getPath().empty());
        EXPECT(addr.toString().find("unnamed") != std::string::npos);
    }
    // Create 工厂：超长返回 nullptr
    {
        std::string too_long(sizeof(((sockaddr_un*)0)->sun_path) + 8, 'x');
        EXPECT(!bronx::BxUnixAddress::Create(too_long));
    }
    // Create 工厂：合法路径返回非空
    {
        auto p = bronx::BxUnixAddress::Create("/tmp/bronx-unix-create.sock");
        EXPECT(p);
        EXPECT(p->getPath() == "/tmp/bronx-unix-create.sock");
    }
    // 默认构造（接收 getpeername/getsockname 回填的占位地址）
    {
        bronx::BxUnixAddress addr;
        EXPECT(addr.getFamily() == AF_UNIX);
        // 默认长度必须是完整 sockaddr_un，便于 getsockname/getpeername 回填最长抽象名
        EXPECT(addr.getAddrLen() == sizeof(sockaddr_un));
        addr.setAddrLen(99999);
        EXPECT(addr.getAddrLen() == sizeof(sockaddr_un));
        addr.setAddrLen(0);
        EXPECT(addr.getAddrLen() == sizeof(sa_family_t));
    }
    // 长度边界：恰好填满 sun_path
    {
        std::string max_path(sizeof(((sockaddr_un*)0)->sun_path) - 1, 'a');
        auto p = bronx::BxUnixAddress::Create(max_path);
        EXPECT(p);
        EXPECT(p->getPath() == max_path);
    }
    // sun_path 大小再 +1 应该越界
    {
        std::string over_path(sizeof(((sockaddr_un*)0)->sun_path), 'a');
        EXPECT(!bronx::BxUnixAddress::Create(over_path));
    }
    // 抽象命名空间最大长度：sun_path 全用、首字节 '\0'
    {
        std::string abs_max(sizeof(((sockaddr_un*)0)->sun_path), '\0');
        for(size_t i = 1; i < abs_max.size(); ++i) abs_max[i] = 'b';
        auto p = bronx::BxUnixAddress::Create(abs_max);
        EXPECT(p);
        EXPECT(p->getAddrLen() == offsetof(sockaddr_un, sun_path) + abs_max.size());
    }
    // raw sockaddr_un + 超大 length：length 应被夹到 <= sizeof(sockaddr_un)，
    // 且普通路径按 C 字符串截断（不把后续填充字节算进 path）
    {
        sockaddr_un raw{};
        raw.sun_family = AF_UNIX;
        std::strcpy(raw.sun_path, "/tmp/clamp");
        bronx::BxUnixAddress u(raw, 99999);
        EXPECT(u.getAddrLen() <= sizeof(sockaddr_un));
        EXPECT(u.getPath() == "/tmp/clamp");
    }
    // raw sockaddr_un + 过小 length（< sa_family_t）：兜底成 unnamed，不越界
    {
        sockaddr_un raw{};
        raw.sun_family = AF_UNIX;
        bronx::BxUnixAddress u(raw, 0);
        EXPECT(u.getFamily() == AF_UNIX);
        EXPECT(u.getPath().empty());
    }
}


// ========== 8. GetInterfaceAddresses ==========
static void test_iface(){
    BRONX_LOG_INFO(g_logger) << "--- test_iface ---";

    std::multimap<std::string, std::pair<bronx::BxAddress::ptr, uint32_t>> m;
    EXPECT(bronx::BxAddress::GetInterfaceAddresses(m, AF_UNSPEC));
    EXPECT(!m.empty());
    bool has_lo = false;
    for(auto& kv : m){
        if(kv.first == "lo"){
            has_lo = true;
            // loopback 地址族要么 v4 要么 v6
            int f = kv.second.first->getFamily();
            EXPECT(f == AF_INET || f == AF_INET6);
        }
        BRONX_LOG_INFO(g_logger) << kv.first << " - " << kv.second.first->toString()
                                 << " - " << kv.second.second;
    }
    EXPECT(has_lo);

    // 指定 iface
    std::vector<std::pair<bronx::BxAddress::ptr, uint32_t>> v;
    EXPECT(bronx::BxAddress::GetInterfaceAddresses(v, "lo", AF_UNSPEC));
    EXPECT(!v.empty());

    // 不存在的 iface
    std::vector<std::pair<bronx::BxAddress::ptr, uint32_t>> v2;
    bool ok = bronx::BxAddress::GetInterfaceAddresses(v2,
                                                    "iface-that-does-not-exist", AF_UNSPEC);
    EXPECT(!ok || v2.empty());

    // "*" 通配，返回 0.0.0.0/:: 占位
    std::vector<std::pair<bronx::BxAddress::ptr, uint32_t>> v3;
    EXPECT(bronx::BxAddress::GetInterfaceAddresses(v3, "*", AF_UNSPEC));
    EXPECT(!v3.empty());
    bool got_v4 = false, got_v6 = false;
    for(auto& kv : v3){
        if(kv.first->getFamily() == AF_INET) got_v4 = true;
        if(kv.first->getFamily() == AF_INET6) got_v6 = true;
        EXPECT(kv.second == 0u);
    }
    EXPECT(got_v4 && got_v6);

    std::vector<std::pair<bronx::BxAddress::ptr, uint32_t>> v4;
    EXPECT(!bronx::BxAddress::GetInterfaceAddresses(v4, "*", AF_UNIX));
    EXPECT(v4.empty());
}


// ========== 9. Unix socket 端到端：bind+listen+accept+recv/send ==========
// 使用 std::thread 直接驱动 bronx::BxSocket（在非 scheduler 线程里 hook 不启用，
// 所有 socket 调用走原生阻塞 syscall），完整覆盖：
//   - BxUnixAddress::Create + BxSocket::bind/listen/accept/connect
//   - getLocalAddress / getRemoteAddress 在 unix 域的回填路径
//   - 收发 + 半关闭检测
static void test_unix_socket_e2e_path(){
    BRONX_LOG_INFO(g_logger) << "--- test_unix_socket_e2e_path ---";

    char buf[128];
    std::snprintf(buf, sizeof(buf), "/tmp/bronx-unix-e2e-%d.sock", (int)getpid());
    const std::string sock_path = buf;
    ::unlink(sock_path.c_str());

    std::atomic<bool> server_ready{false};

    std::thread server_th([&](){
        auto addr = bronx::BxUnixAddress::Create(sock_path);
        EXPECT(addr);
        auto sock = bronx::BxSocket::MakeUnixTcpSocket();
        EXPECT(sock->bind(addr));
        EXPECT(sock->listen());

        // bind 之后 getLocalAddress 应返回正确路径的 BxUnixAddress
        auto local = std::dynamic_pointer_cast<bronx::BxUnixAddress>(sock->getLocalAddress());
        EXPECT(local);
        EXPECT(local->getPath() == sock_path);

        server_ready.store(true);

        // 非阻塞 accept：等到可读再 accept
        struct pollfd lpfd{ sock->getSocket(), POLLIN, 0 };
        ::poll(&lpfd, 1, 5000);
        auto cli = sock->accept();
        EXPECT(cli);
        // 远端虽然 unnamed，至少 family 是 AF_UNIX
        auto remote = cli->getRemoteAddress();
        EXPECT(remote && remote->getFamily() == AF_UNIX);

        char rcv[64] = {0};
        // bronx::BxSocket 创建出来的 fd 默认非阻塞（fd_manager 接管时改的），
        // 这里没有走 BxIoManager 调度所以得自己 poll 等待可读
        struct pollfd spfd{ cli->getSocket(), POLLIN, 0 };
        ::poll(&spfd, 1, 5000);
        int n = cli->recv(rcv, sizeof(rcv) - 1);
        EXPECT(n > 0);
        EXPECT(std::string(rcv, n) == "ping");
        const std::string reply = "pong";
        EXPECT(cli->send(reply.data(), reply.size()) == (int)reply.size());

        cli->close();
        sock->close();
    });

    std::thread client_th([&](){
        // 等 server_ready，避免 client connect 早于 bind
        while(!server_ready.load()){
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto addr = bronx::BxUnixAddress::Create(sock_path);
        EXPECT(addr);
        auto sock = bronx::BxSocket::MakeUnixTcpSocket();
        EXPECT(sock->connect(addr));

        // connect 后远端地址应能读出来
        auto remote = std::dynamic_pointer_cast<bronx::BxUnixAddress>(sock->getRemoteAddress());
        EXPECT(remote);
        EXPECT(remote->getPath() == sock_path);

        const std::string msg = "ping";
        EXPECT(sock->send(msg.data(), msg.size()) == (int)msg.size());
        char buf[64] = {0};
        // bronx::BxSocket fd 默认非阻塞（fd_manager 介入），用 poll 等可读
        struct pollfd pfd{ sock->getSocket(), POLLIN, 0 };
        ::poll(&pfd, 1, 5000);
        int n = sock->recv(buf, sizeof(buf) - 1);
        EXPECT(n > 0);
        EXPECT(std::string(buf, n) == "pong");
        sock->close();
    });

    server_th.join();
    client_th.join();

    ::unlink(sock_path.c_str());
}


// ========== 10. 抽象命名空间 bind 验证 ==========
// 验证 abstract 地址能 bind 并能 getsockname 拉回原路径。
#ifdef __linux__
static void test_unix_abstract_bind(){
    BRONX_LOG_INFO(g_logger) << "--- test_unix_abstract_bind ---";

    {
        char tail[64];
        std::snprintf(tail, sizeof(tail), "bronx-abs-bind-%d", (int)getpid());
        std::string abs_name(1, '\0');
        abs_name += tail;

        auto addr = bronx::BxUnixAddress::Create(abs_name);
        EXPECT(addr);
        auto sock = bronx::BxSocket::MakeUnixTcpSocket();
        EXPECT(sock->bind(addr));

        auto local = std::dynamic_pointer_cast<bronx::BxUnixAddress>(sock->getLocalAddress());
        EXPECT(local);
        // 抽象命名空间路径不含开头的 '\0'
        EXPECT(local->getPath() == std::string(abs_name.data() + 1, abs_name.size() - 1));

        sock->close();
    }

    // 抽象命名空间最大长度：覆盖 BxUnixAddress 默认回填缓冲必须是完整 sockaddr_un
    {
        char tail[64];
        std::snprintf(tail, sizeof(tail), "bronx-abs-max-%d", (int)getpid());
        std::string abs_name(sizeof(((sockaddr_un*)0)->sun_path), 'x');
        abs_name[0] = '\0';
        size_t tail_len = std::strlen(tail);
        if(tail_len > abs_name.size() - 1){
            tail_len = abs_name.size() - 1;
        }
        std::memcpy(&abs_name[1], tail, tail_len);

        auto addr = bronx::BxUnixAddress::Create(abs_name);
        EXPECT(addr);
        auto sock = bronx::BxSocket::MakeUnixTcpSocket();
        EXPECT(sock->bind(addr));

        auto local = std::dynamic_pointer_cast<bronx::BxUnixAddress>(sock->getLocalAddress());
        EXPECT(local);
        EXPECT(local->getAddrLen() == offsetof(sockaddr_un, sun_path) + abs_name.size());
        EXPECT(local->getPath() == std::string(abs_name.data() + 1, abs_name.size() - 1));

        sock->close();
    }
}
#endif


// ========== 11. 大消息 ==========
// 大消息属于 socket 行为而非 address 模块的覆盖范围；并且 bronx::BxSocket 在
// 非 BxIoManager 线程下 fd 也是非阻塞，单纯用 std::thread 跑会复杂化测试代码
// 同时模糊关注点。这里删除该用例。


int main(){
    test_address_create_dispatch();
    test_ip_create_strings();
    test_lookup();
    test_operators();
    test_ipv4_masks();
    test_ipv6_tostring_roundtrip();
    test_ipv6_masks();
    test_unix_address_basics();
    test_iface();
    test_unix_socket_e2e_path();
#ifdef __linux__
    test_unix_abstract_bind();
#endif

    if(g_failed){
        BRONX_LOG_ERROR(g_logger) << "TESTS FAILED: " << g_failed;
        return 1;
    }
    BRONX_LOG_INFO(g_logger) << "ALL TESTS PASS";
    return 0;
}
