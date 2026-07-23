#pragma once

#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <unistd.h>


namespace bronx{

class BxIpAddress;

// 地址抽象基类,把各种 sockaddr(IPv4/IPv6/Unix/未知)包成能比较、能打印的对象。
// getAddr()/getAddrLen() 拿出来直接喂 socket 系统调用。就是个值对象,不带 fd。
// getAddr() 有 const 和非 const 两版,后者留给 getsockname/getpeername 回填。
class BxAddress{
public:
    using ptr = std::shared_ptr<BxAddress>;

    virtual ~BxAddress(){}

    // 返回协议族
    int getFamily() const;

    // 根据地址族信息返回一个包含子类信息的基类指针
    static BxAddress::ptr Create(const sockaddr* addr, socklen_t addrlen);

    // 通过host地址返回对应条件的所有Address
    // result: 保存满足条件的Address
    // host: 域名,服务器名等.举例: www.bronx.top[:80] (方括号为可选内容)
    // family: 协议族(AF_INT, AF_INT6, AF_UNIX)
    // type: socketl类型SOCK_STREAM、SOCK_DGRAM 等
    // protocol: 协议,IPPROTO_TCP、IPPROTO_UDP 等
    static bool Lookup(std::vector<BxAddress::ptr> &result, const std::string &host,
                       int family = AF_INET, int type = 0, int protocol = 0);

    // 通过host地址返回对应条件的任意Address
    static BxAddress::ptr LookupAny(const std::string &host,
                                  int family = AF_INET, int type = 0, int protocol = 0);

    // 通过host地址返回对应条件的任意IPAddress
    static std::shared_ptr<BxIpAddress> ResolveOneIp(const std::string &host,
                                                         int family = AF_INET, int type = 0, int protocol = 0);


    // 返回本机所有网卡的<网卡名, 地址, 子网掩码位数>
    //  map中 key=网卡名，value=<地址，子网掩码位数>
    // result 保存本机所有地址
    // family: 协议族(AF_INT, AF_INT6, AF_UNIX)
    static bool GetInterfaceAddresses(std::multimap<std::string, std::pair<BxAddress::ptr, uint32_t>> &result,
                                      int family = AF_INET);

    // 获取指定网卡的地址和子网掩码位数
    // result: 保存指定网卡所有地址
    // iface: 网卡名称
    static bool GetInterfaceAddresses(std::vector<std::pair<BxAddress::ptr, uint32_t>> &result, const std::string &iface, int family = AF_INET);

    // 返回sockaddr指针，只读
    virtual const sockaddr* getAddr() const = 0;

    // 返回sockaddr指针，读写
    virtual sockaddr* getAddr() = 0;

    // 返回sockaddr的长度
    virtual socklen_t getAddrLen() const = 0;

    // 输出可读性地址到流
    virtual std::ostream& insert(std::ostream& os) const = 0;

    // 返回可读性地址的字符串
    std::string toString() const;

    // 重载关系运算符
    bool operator<(const BxAddress& rhs) const;

    bool operator==(const BxAddress& rhs) const;

    bool operator!=(const BxAddress& rhs) const;
};

// IPv4/IPv6 的共同基类,在 BxAddress 上再补端口、广播/网段/子网掩码这些 IP 专属的东西。
// ResolveOneIp 一般返回它,服务端拿它建监听地址(端口可以晚点再设)。
class BxIpAddress: public BxAddress{
public:
    using ptr = std::shared_ptr<BxIpAddress>;

    // 域名转换为一个IP地址
    static BxIpAddress::ptr Create(const char* address, uint16_t port);

    // 获取广播地址
    // prefix_len: 子网掩码位数
    virtual BxIpAddress::ptr broadcastAddress(uint32_t prefix_len) = 0;

    // 获取网段
    virtual BxIpAddress::ptr networkAddress(uint32_t prefix_len) = 0;

    // 获取子网掩码地址
    virtual BxIpAddress::ptr subnetMask(uint32_t prefix_len) = 0;

    // 返回端口号
    virtual uint32_t getPort() const = 0;

    // 设置端口号
    virtual void setPort(uint16_t v) = 0;
};

// IPv4地址类
class BxIpv4Address: public BxIpAddress{
public:
    using ptr = std::shared_ptr<BxIpv4Address>;

    // 构造函数
    BxIpv4Address(const sockaddr_in& address);

    BxIpv4Address(uint32_t address = INADDR_ANY, uint16_t port = 0);

    // 可读字符串转换为IPv4地址
    static BxIpv4Address::ptr Create(const char* address, uint16_t port);
    
    const sockaddr* getAddr() const override;
    sockaddr* getAddr() override;
    socklen_t getAddrLen() const override;
    std::ostream& insert(std::ostream &os) const override;

    BxIpAddress::ptr broadcastAddress(uint32_t prefix_len) override;
    BxIpAddress::ptr networkAddress(uint32_t prefix_len) override;
    BxIpAddress::ptr subnetMask(uint32_t prefix_len) override;
    uint32_t getPort() const override;
    void setPort(uint16_t v) override;
    
private:
    sockaddr_in addr_;
};

// IPv6地址类
class BxIpv6Address: public BxIpAddress{
public:
    using ptr = std::shared_ptr<BxIpv6Address>;

    // 构造函数
    BxIpv6Address();

    BxIpv6Address(const sockaddr_in6& address);

    static BxIpv6Address::ptr Create(const char* address, uint16_t port);

    // 用 16 字节裸地址构造
    BxIpv6Address(const uint8_t address[16], uint16_t port = 0);

    const sockaddr* getAddr() const override;
    sockaddr* getAddr() override;
    socklen_t getAddrLen() const override;
    std::ostream& insert(std::ostream &os) const override;

    BxIpAddress::ptr broadcastAddress(uint32_t prefix_len) override;
    BxIpAddress::ptr networkAddress(uint32_t prefix_len) override;
    BxIpAddress::ptr subnetMask(uint32_t prefix_len) override;
    uint32_t getPort() const override;
    void setPort(uint16_t v) override;
    
private:
    sockaddr_in6 addr_;
};

// UnixSocket地址类
// 支持三种 unix socket 地址形式：
// 1) 路径名：path 为普通文件系统路径
// 2) 抽象命名空间（Linux 扩展）：path 以 '\0' 开头，后续字节为抽象名（非 c-string）
// 3) unnamed：仅 sa_family，没有路径（来自 socketpair / 未 bind 的对端）
class BxUnixAddress: public BxAddress{
public:
    using ptr = std::shared_ptr<BxUnixAddress>;

    // 构造一个完整 sockaddr_un 长度的 unnamed BxUnixAddress，用于配合 getpeername/getsockname
    BxUnixAddress();

    // 用一段路径或抽象名构造，路径过长会抛 std::logic_error
    // 优先使用 Create 工厂（路径过长时返回 nullptr）
    BxUnixAddress(const std::string& path);

    // 使用一段原始 sockaddr_un 与有效长度构造
    BxUnixAddress(const sockaddr_un& addr, socklen_t length);

    // 工厂方法：路径过长返回 nullptr，不抛异常
    static BxUnixAddress::ptr Create(const std::string& path);

    const sockaddr* getAddr() const override;
    sockaddr* getAddr() override;
    socklen_t getAddrLen() const override;
    void setAddrLen(uint32_t v);
    std::string getPath() const;
    std::ostream& insert(std::ostream &os) const override;

private:
    sockaddr_un addr_;
    // length_ 表示当前 sockaddr_un 地址结构中有效内容的大小
    // = offsetof(sockaddr_un, sun_path) + sun_path 中有效字节数
    // unnamed 地址下 length_ == sizeof(sa_family_t)
    socklen_t length_;
};

// 未知地址类
class BxUnknownAddress: public BxAddress{
public:
    using ptr = std::shared_ptr<BxUnknownAddress>;

    // 构造函数
    BxUnknownAddress(int family);

    BxUnknownAddress(const sockaddr& addr);

    const sockaddr* getAddr() const override;
    sockaddr *getAddr() override;
    socklen_t getAddrLen() const override;
    std::ostream& insert(std::ostream &os) const override;
    
private:
    sockaddr addr_;
};

std::ostream& operator<<(std::ostream& os, const BxAddress& addr);

}
