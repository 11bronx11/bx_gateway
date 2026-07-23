#include "sock_stream.h"



namespace bronx{

// 构造函数
BxSocketStream::BxSocketStream(BxSocket::ptr sock, bool ownner)
    : socket_(sock)
    , ownner_(ownner){
}

// 析构函数，如果ownner==true，则对socket进行close
BxSocketStream::~BxSocketStream(){
    if(socket_ && ownner_){
        socket_->close();
    }
}

// 从 socket 读,转手交给 BxSocket::recv。>0 字节数,=0 对端关闭,<0 出错(没连接就 ENOTCONN)。
// 上层 read 最底层就落到这,没数据时在这让出协程。
int BxSocketStream::read(void* buffer, size_t length){
    if(!isConnected()){
        // 未连接:置明确 errno,保证"返回<0必有可辨原因"的契约(见 stream.h)
        errno = ENOTCONN;
        return -1;
    }
    // 调用Socket::recv
    return socket_->recv(buffer, length);
}

int BxSocketStream::read(BxByteArray::ptr ba, size_t length){
    if(!isConnected() || !ba){
        errno = ENOTCONN;
        return -1;
    }
    if(length == 0){
        return 0;
    }
    std::vector<iovec> iovs;
    // 获取ByteArray的写缓存，读取到的数据写入ByteArray
    uint64_t size = ba->peekWriteIovec(iovs, length);
    if(size == 0 || iovs.empty()){
        return -1;
    }
    int rt = socket_->recv(&iovs[0], iovs.size());
    if(rt > 0){
        // 更新m_position与m_size
        ba->setPosition(ba->getPosition() + rt);
    }
    return rt;
}

// 往 socket 写,转手交给 BxSocket::send。>0 已写字节数,<0 出错(没连接就 ENOTCONN)。
// 上层 write 最底层就落到这,发送缓冲满时在这让出协程。
int BxSocketStream::write(void* buffer, size_t length){
    if(!isConnected()){
        errno = ENOTCONN;
        return -1;
    }
    return socket_->send(buffer, length);
}

int BxSocketStream::write(BxByteArray::ptr ba, size_t length){
    if(!isConnected() || !ba){
        errno = ENOTCONN;
        return -1;
    }
    if(length == 0){
        return 0;
    }
    std::vector<iovec> iovs;
    // 获取ByteArray的读缓存，写入的数据读入ByteArray
    uint64_t size = ba->peekReadIovec(iovs, length);
    if(size == 0 || iovs.empty()){
        return -1;
    }
    int rt = socket_->send(&iovs[0], iovs.size());
    if(rt > 0){
        ba->setPosition(ba->getPosition() + rt);
    }
    return rt;
}

// 关闭流
void BxSocketStream::close() {
    if(socket_){
        socket_->close();
    }
}

// 返回是否连接
bool BxSocketStream::isConnected() const {
    return socket_ && socket_->isConnected();
}

}
