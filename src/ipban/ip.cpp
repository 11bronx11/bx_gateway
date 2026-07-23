#include "ip.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>

namespace bronx {
namespace ipban {

// v4-mapped 前缀: 前 10 字节 0, 接着 ff ff, 后 4 字节是真 v4
static bool isV4Mapped(const uint8_t* b16) {
    static const uint8_t pfx[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    return memcmp(b16, pfx, 12) == 0;
}

bool ipFromSockaddr(const sockaddr* sa, Ip& out) {
    if(!sa) return false;
    if(sa->sa_family == AF_INET) {
        auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        out.fam = Fam::V4;
        out.prefix = 32;
        out.bytes.fill(0);
        memcpy(out.bytes.data(), &in->sin_addr, 4);
        return true;
    }
    if(sa->sa_family == AF_INET6) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        const uint8_t* b = in6->sin6_addr.s6_addr;
        if(isV4Mapped(b)) {
            out.fam = Fam::V4;
            out.prefix = 32;
            out.bytes.fill(0);
            memcpy(out.bytes.data(), b + 12, 4);
            return true;
        }
        out.fam = Fam::V6;
        out.prefix = 128;
        memcpy(out.bytes.data(), b, 16);
        return true;
    }
    return false;
}

// 按 prefix 把主机位清零, 让网段的 bytes 规范化
static void maskBytes(std::array<uint8_t,16>& b, int width, uint8_t prefix) {
    for(int i = 0; i < width; ++i) {
        int bitLo = i * 8;
        if(prefix >= bitLo + 8) continue;          // 整字节都在网络位
        if(prefix <= bitLo) { b[i] = 0; continue; } // 整字节都是主机位
        uint8_t keep = prefix - bitLo;              // 高 keep 位保留
        b[i] &= (uint8_t)(0xff << (8 - keep));
    }
}

bool parseCidr(const std::string& s, Ip& out) {
    std::string ip = s;
    int prefix = -1;
    auto slash = s.find('/');
    if(slash != std::string::npos) {
        ip = s.substr(0, slash);
        const std::string p = s.substr(slash + 1);
        if(p.empty()) return false;
        char* end = nullptr;
        long v = strtol(p.c_str(), &end, 10);
        if(end == p.c_str() || *end != '\0' || v < 0 || v > 128) return false;
        prefix = (int)v;
    }
    // 先试 v4 再试 v6
    in_addr a4{};
    if(inet_pton(AF_INET, ip.c_str(), &a4) == 1) {
        if(prefix < 0) prefix = 32;
        if(prefix > 32) return false;
        out.fam = Fam::V4;
        out.prefix = (uint8_t)prefix;
        out.bytes.fill(0);
        memcpy(out.bytes.data(), &a4, 4);
        maskBytes(out.bytes, 4, out.prefix);
        return true;
    }
    in6_addr a6{};
    if(inet_pton(AF_INET6, ip.c_str(), &a6) == 1) {
        if(prefix < 0) prefix = 128;
        if(isV4Mapped(a6.s6_addr) && prefix >= 96) {
            out.fam = Fam::V4;
            out.prefix = (uint8_t)(prefix - 96);
            out.bytes.fill(0);
            memcpy(out.bytes.data(), a6.s6_addr + 12, 4);
            maskBytes(out.bytes, 4, out.prefix);
            return true;
        }
        out.fam = Fam::V6;
        out.prefix = (uint8_t)prefix;
        memcpy(out.bytes.data(), &a6, 16);
        maskBytes(out.bytes, 16, out.prefix);
        return true;
    }
    return false;
}

bool inNet(const Ip& addr, const Ip& net) {
    if(addr.fam != net.fam) return false;
    int width = net.width();
    uint8_t prefix = net.prefix;
    for(int i = 0; i < width; ++i) {
        int bitLo = i * 8;
        if(prefix >= bitLo + 8) {
            if(addr.bytes[i] != net.bytes[i]) return false;
            continue;
        }
        if(prefix <= bitLo) break;   // 剩下全是主机位, 不用比
        uint8_t keep = prefix - bitLo;
        uint8_t m = (uint8_t)(0xff << (8 - keep));
        if((addr.bytes[i] & m) != (net.bytes[i] & m)) return false;
        break;
    }
    return true;
}

bool isTrustedProxy(const Ip& addr, const std::vector<Ip>& trusted) {
    for(const auto& t : trusted) if(inNet(addr, t)) return true;
    return false;
}

static bool parseXffAddr(const std::string& xff, size_t begin, size_t end, Ip& out) {
    while(begin < end && (xff[begin] == ' ' || xff[begin] == '\t')) ++begin;
    while(end > begin && (xff[end - 1] == ' ' || xff[end - 1] == '\t')) --end;
    if(begin == end || xff.find('/', begin) < end) return false;
    if(!parseCidr(xff.substr(begin, end - begin), out)) return false;
    return (out.fam == Fam::V4 && out.prefix == 32)
        || (out.fam == Fam::V6 && out.prefix == 128);
}

bool resolveClientAddr(const Ip& peer, const std::string& xff,
                       const std::vector<Ip>& trusted, Ip& out) {
    out = peer;
    if(!isTrustedProxy(peer, trusted) || xff.empty()) return true;

    size_t end = xff.size();
    for(;;) {
        size_t comma = end == 0 ? std::string::npos : xff.rfind(',', end - 1);
        size_t begin = comma == std::string::npos ? 0 : comma + 1;
        Ip cand;
        if(!parseXffAddr(xff, begin, end, cand)) return false;
        if(!isTrustedProxy(cand, trusted)) {
            out = cand;
            return true;
        }
        if(comma == std::string::npos) break;
        end = comma;
    }
    return true;
}

Ip resolveClientAddr(const Ip& peer, const std::string& xff, const std::vector<Ip>& trusted) {
    Ip out;
    resolveClientAddr(peer, xff, trusted, out);
    return out;
}

std::string Ip::toString() const {
    char buf[64] = {0};
    if(fam == Fam::V4) {
        inet_ntop(AF_INET, bytes.data(), buf, sizeof(buf));
    } else {
        inet_ntop(AF_INET6, bytes.data(), buf, sizeof(buf));
    }
    std::string s = buf;
    bool full = (fam == Fam::V4 && prefix == 32) || (fam == Fam::V6 && prefix == 128);
    if(!full) s += "/" + std::to_string((int)prefix);
    return s;
}

size_t IpHash::operator()(const Ip& ip) const {
    // FNV-1a, 只吃有效字节 + fam
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint8_t b) { h ^= b; h *= 1099511628211ull; };
    mix((uint8_t)ip.fam);
    int width = ip.width();
    for(int i = 0; i < width; ++i) mix(ip.bytes[i]);
    return (size_t)h;
}

} // namespace ipban
} // namespace bronx
