#include "endpoint.h"
#include "log.h"
#include "_endian.h"
#include <algorithm>
#include <bit>
#include <sstream>
#include <netdb.h>
#include <cstring>
#include <cstddef>
#include <ifaddrs.h>
static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");
namespace bronx{

// 统计掩码里 1 的位数(前缀长度)。用 C++20 std::popcount,单指令级,替手写循环。
template<typename T>
static uint32_t CountBytes(T value){
    static_assert(std::is_unsigned_v<T>, "popcount requires unsigned");
    return (uint32_t)std::popcount(value);
}

// 创建一个低位为 1、高位为 0 的掩码（host order）
// bits: 高位需要保留为 0 的比特数；低 (sizeof(T)*8 - bits) 位为 1
// 例：CreateMask<uint32_t>(24) = 0x000000FF；CreateMask<uint8_t>(3) = 0x1F
// bits 越界时返回 0 作为兜底，避免移位 UB
template<typename T>
static T CreateMask(uint32_t bits){
    if(bits == 0){
        // 全 1。直接 1<<width 在 C++ 里是 UB，必须分支处理
        return ~T(0);
    }
    if(bits >= sizeof(T) * 8){
        // 高位全 0 → 整个掩码就是 0
        return T(0);
    }
    return (T(1) << (sizeof(T) * 8 - bits)) - 1;
}


// Adress

// 返回协议族
int BxAddress::getFamily() const {
    return getAddr()->sa_family;
}

// 返回可读性地址的字符串
std::string BxAddress::toString() const {
    std::stringstream ss;
    insert(ss);
    return ss.str();
}

// 重载关系运算符
bool BxAddress::operator<(const BxAddress& rhs) const {
    // 先比较字节序，再比较长度
    socklen_t minlen = std::min(getAddrLen(), rhs.getAddrLen());
    int result = memcmp(getAddr(), rhs.getAddr(), minlen);
    if(result < 0){
        return true;
    } else if(result > 0){
        return false;
    } else if(getAddrLen() < rhs.getAddrLen()){
        return true;
    }
    return false;
}

bool BxAddress::operator==(const BxAddress& rhs) const {
    return (getAddrLen() == rhs.getAddrLen()) && (memcmp(getAddr(), rhs.getAddr(), getAddrLen()) == 0);
}

bool BxAddress::operator!=(const BxAddress& rhs) const {
    return !(*this == rhs);
}

// 裹一层 getaddrinfo,把 host 解析成所有符合条件的 BxAddress,追加进 result。
// host 支持 "域名"/"ip"/"ip:port"/"[ipv6]:port" 好几种写法。解出东西返回 true。
// 建链的前置步骤,LookupAny 系列的底层,产物拿去 bind/connect。
bool BxAddress::Lookup(std::vector<BxAddress::ptr> &result, const std::string &host, int family, int type, int protocol){
    // 调用getaddrinfo进行查询
    addrinfo hints, *results, *next;
    // hints设置查询条件
    hints.ai_addr = NULL;
    hints.ai_addrlen = 0;
    hints.ai_canonname = NULL;
    hints.ai_family = family;
    hints.ai_flags = 0;
    hints.ai_next = NULL;
    hints.ai_protocol = protocol;
    hints.ai_socktype = type;

    // node存放地址
    std::string node;
    // service存放服务（端口号、协议名等）
    const char* service = NULL;

    // 检查IPv6格式
    // ipv6的地址示例：http://[2001:0:3238:E1:0063::FEFB]:80
    if(!host.empty() && host[0] == '['){
        // memchr结果指向第一个"]"出现的位置
        // 注意搜索长度要用 host.size()：当 ']' 是最后一个字符（无端口的 [::1]）时，
        // 用 size()-1 会漏掉它，导致 IPv6 无端口地址解析失败。
        const char* endipv6 = (const char*)memchr(host.c_str(), ']', host.size());
        if(endipv6){
            if(*(endipv6 + 1) == ':'){
                // 获取端口号（:后的部分）
                service = endipv6 + 2;
            } else if(*(endipv6 + 1) != '\0'){
                // ']' 后只能是字符串结束或 ":port"，否则属于非法 host 形式
                return false;
            }
            // 获取地址（[]内的部分）
            node = host.substr(1, endipv6 - host.c_str() - 1);      // 指针运算
        }
    }

    // 检查常规格式（IPv4）
    if(node.empty()){
        service = (const char*)memchr(host.c_str(), ':', host.size());
        if(service){
            // 检查host中是否只有一个:
            if(!memchr(service + 1, ':', host.c_str() + host.size() - 1 - service)){
                // 格式正常
                node = host.substr(0, service - host.c_str());
                ++service;
            }
        }
    }

    // 特殊格式，没有service
    if(node.empty()){
        node = host;
    }

    // 调用getaddrinfo
    int error = getaddrinfo(node.c_str(), service, &hints, &results);
    if(error){
        BRONX_LOG_DEBUG(g_logger) << "BxAddress::Lookup getaddress(" << host << ", "
                                  << family << ", " << type << ") err=" << error << " errstr="
                                  << gai_strerror(error);
        return false;
    }

    // 解析查询结果
    // 由于results需要保持下来之后freeaddrinfo，所以使用next遍历
    for(next = results; next; next = next->ai_next){
        result.push_back(BxAddress::Create(next->ai_addr, next->ai_addrlen));
        // BRONX_LOG_DEBUG(g_logger) << "family:" << next->ai_family << ", sock type:" << next->ai_socktype;
    }
    freeaddrinfo(results);
    return !result.empty();
}

// 解析 host,取 Lookup 结果的第一个返回,没解析出来就 nullptr。
BxAddress::ptr BxAddress::LookupAny(const std::string &host, int family, int type, int protocol){
    std::vector<BxAddress::ptr> result;
    if(Lookup(result, host, family, type, protocol)){
        return result[0];
    }
    return nullptr;
}

// 解析 host,从结果里挑第一个能转成 BxIpAddress 的返回(拿到后可 setPort 再 bind/connect)。
// 服务端最常用的"域名/IP 字符串 -> 可监听地址"入口。
std::shared_ptr<BxIpAddress> BxAddress::ResolveOneIp(const std::string &host, int family, int type, int protocol){
    std::vector<BxAddress::ptr> result;
    if(Lookup(result, host, family, type, protocol)){
        for(auto& i : result){
            BxIpAddress::ptr v = std::dynamic_pointer_cast<BxIpAddress>(i);
            if(v){
                return v;
            }
        }
    }
    return nullptr;
}


// 返回本机所有网卡的<网卡名, 地址, 子网掩码位数>
bool BxAddress::GetInterfaceAddresses(std::multimap<std::string, std::pair<BxAddress::ptr, uint32_t>> &result, int family){
    // 处理的流程类似Lookup
    // 调用getifaddrs获取网卡相关信息
    struct ifaddrs *results, *next;
    if(getifaddrs(&results) != 0){
        BRONX_LOG_ERROR(g_logger) << "BxAddress::GetInterfaceAddresses getifaddrs "
                                  << " err=" << errno << " errstr=" << strerror(errno);
        return false;
    }

    try{
        for(next = results; next; next = next->ifa_next){
            BxAddress::ptr addr;
            uint32_t prefix_len = ~0u;
            // 一些虚拟接口（如 wireguard 上行未配置时）ifa_addr 可能为 NULL
            if(!next->ifa_addr){
                continue;
            }
            if(family != AF_UNSPEC && next->ifa_addr->sa_family != family){
                // 协议族不符合要求
                continue;
            }
            // 根据协议族创建网卡地址对象
            switch(next->ifa_addr->sa_family){
            case AF_INET: {
                addr = BxAddress::Create(next->ifa_addr, sizeof(sockaddr_in));
                if(next->ifa_netmask){
                    uint32_t& netmask = ((sockaddr_in*)next->ifa_netmask)->sin_addr.s_addr;
                    prefix_len = CountBytes(netmask);
                } else {
                    prefix_len = 0;
                }
            } break;
            case AF_INET6: {
                addr = BxAddress::Create(next->ifa_addr, sizeof(sockaddr_in6));
                prefix_len = 0;
                if(next->ifa_netmask){
                    in6_addr& netmask = ((sockaddr_in6*)next->ifa_netmask)->sin6_addr;
                    for(int i = 0; i < 16; ++i){
                        prefix_len += CountBytes(netmask.s6_addr[i]);
                    }
                }
            } break;
            default:
                break;
            }

            if(addr){
                result.insert(std::make_pair(next->ifa_name,
                                             std::make_pair(addr, prefix_len)));
            }
        }
    } catch(...){
        BRONX_LOG_ERROR(g_logger) << "BxAddress::GetInterfaceAddresses exception";
        freeifaddrs(results);
        return false;
    }
    freeifaddrs(results);
    return !result.empty();
}

// 获取指定网卡的地址和子网掩码位数
bool BxAddress::GetInterfaceAddresses(std::vector<std::pair<BxAddress::ptr, uint32_t>> &result, const std::string &iface, int family){
    if(iface.empty() || iface == "*"){
        // 不指定网卡：返回通配地址（0.0.0.0 / ::）
        if(family == AF_INET || family == AF_UNSPEC){
            result.push_back(std::make_pair(BxAddress::ptr(new BxIpv4Address()), 0u));
        }
        if(family == AF_INET6 || family == AF_UNSPEC){
            result.push_back(std::make_pair(BxAddress::ptr(new BxIpv6Address()), 0u));
        }
        return !result.empty();
    }

    std::multimap<std::string, std::pair<BxAddress::ptr, uint32_t>> results;
    if(!GetInterfaceAddresses(results, family)){
        return false;
    }

    // equal_range返回匹配的区间
    auto its = results.equal_range(iface);
    for(; its.first != its.second; ++its.first){
        result.push_back(its.first->second);
    }
    return !result.empty();
}


// 工厂:看 sockaddr 的协议族,造出对应的 BxAddress 子类。会校验各族的最小长度,
// addr 为空或长度不够返回 nullptr。BxSocket 回填地址、Lookup 解析都经这产出具体对象。
BxAddress::ptr BxAddress::Create(const sockaddr* addr, socklen_t addrlen){
    if(!addr){
        return nullptr;
    }
    if(addrlen < sizeof(sa_family_t)){
        return nullptr;
    }

    // 根据地址族返回准确的IP地址类型
    BxAddress::ptr result;
    switch(addr->sa_family){
    case AF_INET:
        if(addrlen < sizeof(sockaddr_in)){
            return nullptr;
        }
        result = std::make_shared<BxIpv4Address>(*(const sockaddr_in*)addr);
        break;
    case AF_INET6:
        if(addrlen < sizeof(sockaddr_in6)){
            return nullptr;
        }
        result = std::make_shared<BxIpv6Address>(*(const sockaddr_in6*)addr);
        break;
    case AF_UNIX: {
        // unix 地址有效长度是变长的（取决于 sun_path 实际占多少字节），
        // 所以需要把外部传入的 addrlen 一起带过去
        sockaddr_un unaddr;
        memset(&unaddr, 0, sizeof(unaddr));
        socklen_t copy_len = std::min<socklen_t>(addrlen, sizeof(unaddr));
        memcpy(&unaddr, addr, copy_len);
        result = std::make_shared<BxUnixAddress>(unaddr, copy_len);
        break;
    }
    default:
        if(addrlen >= sizeof(sockaddr)){
            result = std::make_shared<BxUnknownAddress>(*addr);
        } else {
            result = std::make_shared<BxUnknownAddress>(addr->sa_family);
        }
        break;
    }
    return result;
}

// BxIpAddress
// 域名转换为一个IP地址
BxIpAddress::ptr BxIpAddress::Create(const char* address, uint16_t port){
    if(!address){
        return nullptr;
    }

    addrinfo hints, *results;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;        // IPv4或IPv6
    hints.ai_flags = AI_NUMERICHOST;    // 不进行名字解析

    // 调用getaddrinfo
    int error = getaddrinfo(address, NULL, &hints, &results);
    if(error){
        BRONX_LOG_ERROR(g_logger) << "BxIpAddress::Create(" << address << ", "
                                  << port <<") error=" << error
                                  << " errstr=" << gai_strerror(error);
        return nullptr;
    }

    // 调用Address的Create方法获取准确的IP地址类（需要dynamic_cast）
    try{
        BxIpAddress::ptr result = std::dynamic_pointer_cast<BxIpAddress>(
            BxAddress::Create(results->ai_addr, results->ai_addrlen));
        if(result){
            result->setPort(port);
        }
        // 使用完getaddrinfo后需要调用freeaddrinfo释放
        freeaddrinfo(results);
        return result;
    } catch(...){
        freeaddrinfo(results);
        return nullptr;
    }
}

// IPv4
BxIpv4Address::BxIpv4Address(const sockaddr_in& address)
    : addr_(address){
}

BxIpv4Address::BxIpv4Address(uint32_t address, uint16_t port){
    memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    // 转为大端字节序（网络字节顺序采用大端排序方式）
    addr_.sin_port = byteswapToBigEndian(port);
    addr_.sin_addr.s_addr = byteswapToBigEndian(address);
}

// 可读字符串转换为IPv4地址
BxIpv4Address::ptr BxIpv4Address::Create(const char* address, uint16_t port){
    if(!address){
        return nullptr;
    }

    // 调用 inet_pton 将字符串转换为IPv4地址
    BxIpv4Address::ptr rt(new BxIpv4Address);
    rt->addr_.sin_port = byteswapToBigEndian(port);
    int res = inet_pton(AF_INET, address, &rt->addr_.sin_addr);
    if(res <= 0){
        BRONX_LOG_ERROR(g_logger) << "BxIpv4Address::Create(" << address << ", "
                                  << port << ") rt=" << res << " errno=" << errno
                                  << " errstr=" << strerror(errno);
        return nullptr;
    }
    return rt;
}

const sockaddr* BxIpv4Address::getAddr() const {
    return (sockaddr*)&addr_;
}

sockaddr* BxIpv4Address::getAddr(){
    return (sockaddr*)&addr_;
}

socklen_t BxIpv4Address::getAddrLen() const {
    return sizeof(addr_);
}

std::ostream& BxIpv4Address::insert(std::ostream &os) const {
    uint32_t addr = byteswapToBigEndian(addr_.sin_addr.s_addr);
    // int转为ipv4地址
    //  -&0xff：屏蔽掉其他位，只保留最低 8 位
    os << ((addr >> 24) & 0xff) << "."
       << ((addr >> 16) & 0xff) << "."
       << ((addr >> 8) & 0xff) << "."
       << (addr & 0xff);
    // port
    os << ":" << byteswapToBigEndian(addr_.sin_port);
    return os;
}

// 广播地址低位为全1（如192.168.255.255）
BxIpAddress::ptr BxIpv4Address::broadcastAddress(uint32_t prefix_len){
    if(prefix_len > 32){
        return nullptr;
    }

    sockaddr_in baddr(addr_);
    // IPv4地址与掩码做位或操作，得到广播地址（低位全1）
    baddr.sin_addr.s_addr |= byteswapToBigEndian(CreateMask<uint32_t>(prefix_len));
    return std::make_shared<BxIpv4Address>(baddr);
}

// 网段为高位全1（如255.255.137.135）
BxIpAddress::ptr BxIpv4Address::networkAddress(uint32_t prefix_len){
    if(prefix_len > 32){
        return nullptr;
    }

    sockaddr_in naddr(addr_);
    // IPv4地址与掩码取反做位与操作，得到网段地址（高位全1）
    naddr.sin_addr.s_addr &= byteswapToBigEndian(~CreateMask<uint32_t>(prefix_len));
    return std::make_shared<BxIpv4Address>(naddr);
}

// 子网掩码，高位全1低位全0（如255.255.0.0）
BxIpAddress::ptr BxIpv4Address::subnetMask(uint32_t prefix_len){
    if(prefix_len > 32){
        return nullptr;
    }
    sockaddr_in subnet;
    memset(&subnet, 0, sizeof(subnet));
    subnet.sin_family = AF_INET;
    // s_addr 是网络字节序，host-order 的掩码必须先转成 big-endian
    subnet.sin_addr.s_addr = byteswapToBigEndian(~CreateMask<uint32_t>(prefix_len));
    return std::make_shared<BxIpv4Address>(subnet);
}

uint32_t BxIpv4Address::getPort() const {
    return byteswapToBigEndian(addr_.sin_port);
}

void BxIpv4Address::setPort(uint16_t v){
    addr_.sin_port = byteswapToBigEndian(v);
}


// IPv6
BxIpv6Address::BxIpv6Address(){
    memset(&addr_, 0, sizeof(addr_));
    addr_.sin6_family = AF_INET6;
}

BxIpv6Address::BxIpv6Address(const sockaddr_in6& address)
    : addr_(address){
}

BxIpv6Address::BxIpv6Address(const uint8_t address[16], uint16_t port){
    memset(&addr_, 0, sizeof(addr_));
    addr_.sin6_family = AF_INET6;
    addr_.sin6_port = byteswapToBigEndian(port);
    memcpy(&addr_.sin6_addr.s6_addr, address, 16);
}

// 可读字符串转换为IPv6地址
BxIpv6Address::ptr BxIpv6Address::Create(const char* address, uint16_t port){
    if(!address){
        return nullptr;
    }

    // 调用 inet_pton 将字符串转换为IPv6地址
    BxIpv6Address::ptr rt(new BxIpv6Address);
    rt->addr_.sin6_port = byteswapToBigEndian(port);
    int res = inet_pton(AF_INET6, address, &rt->addr_.sin6_addr);
    if(res <= 0){
        BRONX_LOG_ERROR(g_logger) << "BxIpv6Address::Create(" << address << ", "
                                  << port << ") rt=" << res << " errno=" << errno
                                  << " errstr=" << strerror(errno);
        return nullptr;
    }
    return rt;
}

const sockaddr* BxIpv6Address::getAddr() const {
    return (sockaddr*)&addr_;
}

sockaddr* BxIpv6Address::getAddr(){
    return (sockaddr*)&addr_;
}

socklen_t BxIpv6Address::getAddrLen() const {
    return sizeof(addr_);
}

std::ostream& BxIpv6Address::insert(std::ostream &os) const {
    os << "[";
    // 地址按 16 位（2 字节）一组，共 8 组
    uint16_t groups[8];
    for(int i = 0; i < 8; ++i){
        groups[i] = ((uint16_t)addr_.sin6_addr.s6_addr[i * 2] << 8)
                  | (uint16_t)addr_.sin6_addr.s6_addr[i * 2 + 1];
    }

    // 先找出最长的连续全 0 区间 [best_start, best_start+best_len)
    // 按 RFC 5952：只有长度 >= 2 的零段才用 :: 压缩；多个等长时取第一个。
    int best_start = -1, best_len = 0;
    int cur_start = -1, cur_len = 0;
    for(int i = 0; i < 8; ++i){
        if(groups[i] == 0){
            if(cur_start < 0){
                cur_start = i;
                cur_len = 1;
            } else {
                ++cur_len;
            }
            if(cur_len > best_len){
                best_len = cur_len;
                best_start = cur_start;
            }
        } else {
            cur_start = -1;
            cur_len = 0;
        }
    }
    if(best_len < 2){
        // 没有可压缩的零段
        best_start = -1;
    }

    // 逐组输出。压缩段用一个空 token 表示，最终靠 "::" 体现。
    // 算法：正常组之间用单冒号分隔；遇到压缩段，输出到目前为止内容后接 "::"，
    // 再输出压缩段之后的组（这些组之间仍用单冒号），保证全程只有压缩处出现双冒号。
    bool first = true;          // 是否还没输出过任何字符（控制前导冒号）
    for(int i = 0; i < 8; ){
        if(best_start >= 0 && i == best_start){
            // 压缩段：直接补 "::"。无论它在开头、中间还是结尾，
            // "::" 自身已经包含了它两侧所需的冒号。
            os << "::";
            i += best_len;
            first = true;       // "::" 末尾已有冒号，下一组不再加前导冒号
            continue;
        }
        if(!first){
            os << ":";
        }
        os << std::hex << (int)groups[i] << std::dec;
        first = false;
        ++i;
    }

    os << "]:" << byteswapToBigEndian(addr_.sin6_port);
    return os;
}

BxIpAddress::ptr BxIpv6Address::broadcastAddress(uint32_t prefix_len){
    // 注意：s6_addr中，索引靠前的部分为IPv6地址的高位，索引靠后的为低位
    // 高位不变，仅对索引为 [prefix_len / 8] 的字节施加掩码操作
    if(prefix_len > 128){
        return nullptr;
    }
    sockaddr_in6 baddr(addr_);
    if(prefix_len < 128){
        baddr.sin6_addr.s6_addr[prefix_len / 8] |= CreateMask<uint8_t>(prefix_len % 8);
        // 低位变为全1
        for(int i = prefix_len / 8 + 1; i < 16; ++i){
            baddr.sin6_addr.s6_addr[i] = 0xff;
        }
    }
    // /128：单点路由，broadcast 等于自身
    return std::make_shared<BxIpv6Address>(baddr);
}

BxIpAddress::ptr BxIpv6Address::networkAddress(uint32_t prefix_len){
    if(prefix_len > 128){
        return nullptr;
    }
    sockaddr_in6 naddr(addr_);
    if(prefix_len < 128){
        // 分界字节：保留高 (prefix_len%8) 位、清掉低位 → 与 ~mask 做与
        naddr.sin6_addr.s6_addr[prefix_len / 8] &= ~CreateMask<uint8_t>(prefix_len % 8);
        // 低位变为全0
        for(int i = prefix_len / 8 + 1; i < 16; ++i){
            naddr.sin6_addr.s6_addr[i] = 0x00;
        }
    }
    return std::make_shared<BxIpv6Address>(naddr);
}

BxIpAddress::ptr BxIpv6Address::subnetMask(uint32_t prefix_len){
    if(prefix_len > 128){
        return nullptr;
    }
    sockaddr_in6 subnet;
    memset(&subnet, 0, sizeof(subnet));
    subnet.sin6_family = AF_INET6;
    // 完整 1 字节的"网络位"段
    for(uint32_t i = 0; i < prefix_len / 8; ++i){
        subnet.sin6_addr.s6_addr[i] = 0xff;
    }
    // 分界字节：高 (prefix_len%8) 位为 1、低位为 0
    if(prefix_len < 128){
        subnet.sin6_addr.s6_addr[prefix_len / 8] = ~CreateMask<uint8_t>(prefix_len % 8);
    }
    return std::make_shared<BxIpv6Address>(subnet);
}

uint32_t BxIpv6Address::getPort() const {
    return byteswapToBigEndian(addr_.sin6_port);
}

void BxIpv6Address::setPort(uint16_t v){
    addr_.sin6_port = byteswapToBigEndian(v);
}


// Unix

// 默认构造：先把 length_ 撑到最大，便于配合 getpeername/getsockname 这类
// 由内核回填地址、再用 setAddrLen 截断的场景
BxUnixAddress::BxUnixAddress(){
    memset(&addr_, 0, sizeof(addr_));
    addr_.sun_family = AF_UNIX;
    length_ = sizeof(addr_);
}

// 用一段路径或抽象名构造
// 三种情况：
//   1) 普通路径："/tmp/xxx.sock"     → 写到 sun_path 并保留末尾 '\0'
//   2) 抽象命名空间："\0name"        → 首字节为 '\0' 时不要再补 '\0'
//   3) 空字符串 ""                    → 视作 unnamed（仅 sa_family）
// 路径过长抛 std::logic_error；优先调用 Create() 拿到 nullptr 不抛异常
BxUnixAddress::BxUnixAddress(const std::string& path){
    memset(&addr_, 0, sizeof(addr_));
    addr_.sun_family = AF_UNIX;

    if(path.empty()){
        length_ = sizeof(sa_family_t);
        return;
    }

    size_t copy_len = path.size();
    if(path[0] != '\0'){
        // 普通路径需要保留 trailing '\0'
        ++copy_len;
    }
    if(copy_len > sizeof(addr_.sun_path)){
        throw std::logic_error("path too long");
    }
    memcpy(addr_.sun_path, path.data(), copy_len);
    length_ = offsetof(sockaddr_un, sun_path) + copy_len;
}

// 直接接收一段已填充好的 sockaddr_un 与有效长度
// 主要供 BxAddress::Create 在 AF_UNIX 分支下使用，length 来自 getpeername/getsockname
BxUnixAddress::BxUnixAddress(const sockaddr_un& addr, socklen_t length)
    : addr_(addr)
    , length_(length){
    // 防越界：内核理论上不会回填超过 sizeof(addr_) 的长度，但稳一点
    if(length_ > sizeof(addr_)){
        length_ = sizeof(addr_);
    }
    if(length_ < sizeof(sa_family_t)){
        length_ = sizeof(sa_family_t);
    }
}

BxUnixAddress::ptr BxUnixAddress::Create(const std::string& path){
    if(path.size() > sizeof(((sockaddr_un*)0)->sun_path)
        || (!path.empty() && path[0] != '\0' && path.size() + 1 > sizeof(((sockaddr_un*)0)->sun_path))){
        return nullptr;
    }
    return std::make_shared<BxUnixAddress>(path);
}

const sockaddr* BxUnixAddress::getAddr() const {
    return (sockaddr*)&addr_;
}

sockaddr* BxUnixAddress::getAddr(){
    return (sockaddr*)&addr_;
}

void BxUnixAddress::setAddrLen(uint32_t v){
    if(v > sizeof(addr_)){
        length_ = sizeof(addr_);
    } else if(v < sizeof(sa_family_t)){
        length_ = sizeof(sa_family_t);
    } else {
        length_ = v;
    }
}


socklen_t BxUnixAddress::getAddrLen() const {
    return length_;
}

std::string BxUnixAddress::getPath() const {
    // unnamed：没有路径
    if(length_ <= offsetof(sockaddr_un, sun_path)){
        return std::string();
    }
    size_t path_len = length_ - offsetof(sockaddr_un, sun_path);
    if(addr_.sun_path[0] == '\0'){
        // 抽象命名空间：首字节是 '\0'，剩下的字节是名字（非 c-string，可能含 0）
        if(path_len <= 1){
            return std::string();
        }
        return std::string(addr_.sun_path + 1, path_len - 1);
    }
    // 普通路径：sun_path 是以 '\0' 结尾的文件系统路径，按 C 字符串截断。
    // 即使 length_ 被夹得偏大（异常构造 / buffer 满长度），也只取到首个 '\0'，
    // 避免把后续填充字节当成路径内容。
    size_t real_len = ::strnlen(addr_.sun_path, path_len);
    return std::string(addr_.sun_path, real_len);
}

std::ostream& BxUnixAddress::insert(std::ostream &os) const {
    if(length_ <= offsetof(sockaddr_un, sun_path)){
        return os << "unix:<unnamed>";
    }
    if(addr_.sun_path[0] == '\0'){
        // 抽象命名空间通常以 @ 前缀展示（lsof / ss / netstat 风格）
        return os << "unix:@" << getPath();
    }
    return os << "unix:" << getPath();
}


// UNKNON
BxUnknownAddress::BxUnknownAddress(int family){
    memset(&addr_, 0, sizeof(addr_));
    addr_.sa_family = family;
}

BxUnknownAddress::BxUnknownAddress(const sockaddr& addr)
    : addr_(addr){
}

const sockaddr* BxUnknownAddress::getAddr() const{
    return &addr_;
}

sockaddr* BxUnknownAddress::getAddr(){
    return &addr_;
}

socklen_t BxUnknownAddress::getAddrLen() const {
    return sizeof(addr_);
}

std::ostream& BxUnknownAddress::insert(std::ostream &os) const {
    os << "[BxUnknownAddress family=" << addr_.sa_family << "]";
    return os;
}


std::ostream& operator<<(std::ostream& os, const BxAddress& addr){
    return addr.insert(os);
}


}
