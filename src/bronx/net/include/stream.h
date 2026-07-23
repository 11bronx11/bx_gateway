#pragma once

#include <memory>
#include "bytearray.h"


namespace bronx{

// 字节流读写的抽象基类,屏蔽底层是 socket、文件还是内存。
// 子类只要实现单次 read/write/close,定长读写(readExact/writeExact)由基类循环补齐。
//
// 读/写返回值语义(全链统一):
//   单次 read/write:
//     > 0  实际处理字节数(可能小于 length,半包/部分写,正常)
//     = 0  对端关闭
//     < 0  出错,看 errno:ETIMEDOUT 超时 / ECANCELED 被取消 / EBADF fd 已关 / 其它 socket 错误
//   readExact/writeExact 循环到满 length:
//     = length 成功;<= 0 失败。注意中途失败不回报已处理字节数,要精确游标就自己用单次读写累加。
class Stream{
public:
    using ptr = std::shared_ptr<Stream>;

    virtual ~Stream(){}

    // 读写数据可以用裸指针,也可以用 BxByteArray 承载

    virtual int read(void* buffer, size_t length) = 0;
    virtual int read(BxByteArray::ptr ba, size_t length) = 0;
    virtual int readExact(void* buffer, size_t length);
    virtual int readExact(BxByteArray::ptr ba, size_t length);

    virtual int write(void* buffer, size_t length) = 0;
    virtual int write(BxByteArray::ptr ba, size_t length) = 0;
    virtual int writeExact(void* buffer, size_t length);
    virtual int writeExact(BxByteArray::ptr ba, size_t length);

    // const 缓冲区的便捷重载
    int writeExact(const void* buffer, size_t length){
        return writeExact(const_cast<void*>(buffer), length);
    }

    virtual void close() = 0;
};


}