#pragma once

#include <memory>
#include <string>
#include <iostream>
#include "endpoint.h"
#include "noncopyable.h"

namespace bronx{

// 把 socket fd 包成对象,框架跟内核网络栈的边界就在这。
// 用静态工厂或 accept() 造出来。recv/send/connect/accept 都经过 hook 变成协程让出版:
// EAGAIN 就挂事件让出,就绪/超时/取消再切回来。
// close() 是幂等的,会去唤醒挂在这个 fd 上的协程(带 ECANCELED 返回),
// BxTcpServer 的 stop/drain 踢连接就靠它。同一个 socket 别多线程并发收发。
class BxSocket: public std::enable_shared_from_this<BxSocket>, public Noncopyable{
public:
    using ptr = std::shared_ptr<BxSocket>;
    using weak_ptr = std::weak_ptr<BxSocket>;

    enum Type{
        TCP = SOCK_STREAM,
        UDP = SOCK_DGRAM
    };

    enum Family{
        IPv4 = AF_INET,
        IPv6 = AF_INET6,
        Unix = AF_UNIX
    };

    // 创建TCP BxSocket（协议族同参数addr）
    static BxSocket::ptr MakeTcp(BxAddress::ptr addr);

    // 创建UDP BxSocket（协议族同参数addr）
    static BxSocket::ptr MakeUdp(BxAddress::ptr addr);

    // 创建IPv4的TCP BxSocket
    static BxSocket::ptr MakeTcpSocket();

    // 创建IPv6的TCP BxSocket
    static BxSocket::ptr MakeTcpSocket6();

    // 创建Unix的TCP BxSocket
    static BxSocket::ptr MakeUnixTcpSocket();

    // 创建IPv4的UDP BxSocket
    static BxSocket::ptr MakeUdpSocket();

    // 创建IPv6的UDP BxSocket
    static BxSocket::ptr MakeUdpSocket6();

    // 创建Unix的UDP BxSocket
    static BxSocket::ptr MakeUnixUdpSocket();

    // 构造函数
    BxSocket(int family, int type, int protocol = 0);

    // 析构函数
    ~BxSocket();

    // 获取发送超时时间
    int64_t getSendTimeout() const;
    // 设置发送超时时间
    void setSendTimeout(int64_t v);

    // 获取接收超时时间
    int64_t getRecvTimeout() const;
    // 设置接收超时时间
    void setRecvTimeout(int64_t v);

    // 获取sockopt
    bool getOption(int level, int optname, void *result, socklen_t *optlen);
    // 获取sockopt（更易用的版本）
    template<typename T>
    bool getOption(int level, int optname, T& result){
        socklen_t len = sizeof(T);
        return getOption(level, optname, &result, &len);
    }

    // 设置sockopt
    bool setOption(int level, int optname, const void *optval, socklen_t optlen);
    // 设置sockopt（更易用的版本）
    template<typename T>
    bool setOption(int level, int optname, const T& optval){
        return setOption(level, optname, &optval, sizeof(T));
    }

    // accept 一个新连接。没连接时协程让出,来连接或被 cancel 再切回。
    // 成功返回初始化好的客户端 socket,失败返回 nullptr(被 abortAll 唤醒退出等)。
    // 监听端用,BxTcpServer 的 accept 协程里循环调它。
    BxSocket::ptr accept();

    // 封装bind，绑定地址(监听端建链第一步,由 BxTcpServer::bind 调用)
    bool bind(const BxAddress::ptr addr);

    // connect 到远端。连接过程中协程让出,完成/超时/取消再切回。
    // addr 会被缓存下来供 reconnect 复用;timeout_ms=-1 不限时。
    // 成功返回 true 并填好本地/远端地址,失败 false 且顺手 close 自己。客户端方向用(反向代理)。
    bool connect(const BxAddress::ptr addr, uint64_t timeout_ms = -1);
    // 重新连接远端地址(复用上次 connect 缓存的 remoteAddress_)
    bool reconnect(uint64_t timeout_ms = -1);

    // 封装listen，监听socket(监听端建链第三步,bind 之后)
    // backlog：未完成连接队列的最大长度
    bool listen(int backlog = SOMAXCONN);

    // 关闭 socket,幂等。先经 owner IoManager 路由 abortAll 唤醒挂在这 fd 上的协程
    // (带 ECANCELED 返回),再从 FdMgr 注销并 ::close。stop/drain 踢连接的底座。
    bool close();

    // 发送数据。发送缓冲满了协程让出。>0 已发字节数,<0 出错。被 BxSocketStream::write 调。
    int send(const void *buf, size_t len, int flags = 0);
    int send(const iovec *buf, size_t len, int flags = 0);
    int sendTo(const void *buf, size_t len, const BxAddress::ptr toAddr, int flags = 0);
    int sendTo(const iovec *buf, size_t len, const BxAddress::ptr toAddr, int flags = 0);

    // 接收数据。没数据时协程让出。>0 字节数,=0 对端关闭,<0 出错。
    // 被 BxSocketStream::read 调,是服务端收数据的最底层落点。
    int recv(void *buf, size_t len, int flags = 0);
    int recv(iovec *buf, size_t len, int flags = 0);
    int recvfrom(void *buf, size_t len, BxAddress::ptr fromAddr, int flags = 0);
    int recvfrom(iovec *buf, size_t len, BxAddress::ptr fromAddr, int flags = 0);

    // 获取远端地址
    BxAddress::ptr getRemoteAddress();
    // 获取本地地址
    BxAddress::ptr getLocalAddress();

    // 获取sockfd
    int getSocket() const { return sock_; }
    // 获取协议族
    int getFamily() const { return family_; }
    // 获取类型
    int getType() const { return type_; }
    // 获取协议
    int getProtocal() const { return protocol_; }
    // 返回是否连接
    bool isConnected() const { return isConnected_; }

    // 返回当前socket是否有效
    bool isValid() const;

    // 返回socket错误
    int getError();

    // 输出信息到流中
    std::ostream& dump(std::ostream& os) const;
    std::string toString() const;

    // 取消读
    bool abortRead();
    // 取消写
    bool abortWrite();
    // 取消accept
    bool abortAccept();
    // 取消所有事件
    bool abortAll();

private:
    // 初始化socket
    void initSock();

    // 创建socket
    void newSock();

    // 接收一个int类型的sockfd来初始化自身
    bool init(int sock);

private:
    // sockfd
    int sock_;
    // 协议族
    int family_;
    // 类型（SOCK_STREAM、SOCK_DGRAM 等）
    int type_;
    // 协议
    int protocol_;
    // 是否连接
    bool isConnected_;
    // 一个connect的本地地址
    BxAddress::ptr localAddress_;
    // 一个connect的远端地址
    BxAddress::ptr remoteAddress_;
};

std::ostream& operator<<(std::ostream& os, const BxSocket& sock);


}