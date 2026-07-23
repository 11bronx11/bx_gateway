#pragma once

// 规范化的 IP 地址,v4 和 v6 都塞进 16 字节里存,v4-mapped 一律归到 v4。
// 热路径查名单只认这个二进制形式,不再拿字符串绕 inet_pton。

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/socket.h>

namespace bronx {
namespace ipban {

enum class Fam : uint8_t { V4, V6 };

// 一个地址或一段网段。prefix 是掩码位数,v4 满位 32,v6 满位 128。
// 单 IP 就是满位。bytes 里 v4 只用前 4 字节。
struct Ip {
    Fam fam = Fam::V4;
    uint8_t prefix = 32;
    std::array<uint8_t, 16> bytes{};

    bool operator==(const Ip& o) const {
        return fam == o.fam && prefix == o.prefix && bytes == o.bytes;
    }
    // 满位掩码下的字节数,v4=4 v6=16
    int width() const { return fam == Fam::V4 ? 4 : 16; }
    std::string toString() const;   // 调试/日志用,不进热路径
};

// 从 sockaddr 直接抠二进制。认 AF_INET / AF_INET6,v4-mapped(::ffff:a.b.c.d)拆回 v4。
// 认出来返回 true,填 out(prefix 置满位);不认返回 false。
bool ipFromSockaddr(const sockaddr* sa, Ip& out);

// 解析 "1.2.3.0/24" 或 "1.2.3.4" 或 "2001:db8::/32"。带 / 就是网段,不带就是单 IP。
// prefix 越界或地址非法返回 false。解析出来的 bytes 已按掩码清掉主机位。
bool parseCidr(const std::string& s, Ip& out);

// addr 落在 net 网段里吗。两者 fam 不同直接 false。热路径按 prefix 逐字节比。
bool inNet(const Ip& addr, const Ip& net);

// addr 命中可信代理名单里任意一条吗。空名单恒 false。
bool isTrustedProxy(const Ip& addr, const std::vector<Ip>& trusted);

// 可信代理才读 XFF, 解析失败返回 false
bool resolveClientAddr(const Ip& peer, const std::string& xff,
                       const std::vector<Ip>& trusted, Ip& out);

// 兼容旧调用, 失败时返回 peer
Ip resolveClientAddr(const Ip& peer, const std::string& xff, const std::vector<Ip>& trusted);

// 精确 IP 的 hash,给 unordered_map 用。只 hash fam + 有效字节。
struct IpHash {
    size_t operator()(const Ip& ip) const;
};
struct IpEq {
    bool operator()(const Ip& a, const Ip& b) const { return a == b; }
};

} // namespace ipban
} // namespace bronx
