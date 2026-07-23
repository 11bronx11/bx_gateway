#pragma once

#include "net_socket.h"
#include "stream.h"


namespace bronx{

// 把 BxSocket 适配成 Stream:上层(HTTP/WS/业务)面向流读写,不直接碰 socket API。
// read/write 内部转调 BxSocket::recv/send。
// ownner=true 时析构会 close 这个 socket(独占);false 只借用不关。
// 读写前都查 isConnected(),没连上就 errno=ENOTCONN 返回 -1。
class BxSocketStream: public Stream{
public:
    BxSocketStream(BxSocket::ptr sock, bool ownner = true);
    ~BxSocketStream();

    virtual int read(void* buffer, size_t length) override;
    virtual int read(BxByteArray::ptr ba, size_t length) override;

    virtual int write(void* buffer, size_t length) override;
    virtual int write(BxByteArray::ptr ba, size_t length) override;

    virtual void close() override;

    BxSocket::ptr getSocket() const { return socket_; }
    bool isConnected() const;
protected:
    BxSocket::ptr socket_;
    bool ownner_;      // 是否独占该 socket 的生命周期
};


}