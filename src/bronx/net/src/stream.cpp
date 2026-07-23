#include "stream.h"


namespace bronx{


// 一直调子类 read() 直到读满 length,中途碰上 EOF 或出错就提前返回。
// 读满返回 length,对端关闭返回 0,出错返回 <0(半包中途挂了不回报已读多少)。
// 协议层要"必须读满"时用它,比如 HTTP 定长 body、WebSocket 帧。
int Stream::readExact(void* buffer, size_t length){
    size_t offset = 0;
    int64_t left = length;
    // 循环读取，把length长度的数据全部读出来
    while(left > 0){
        // 调用 this->read()，利用多态性进行读取
        int64_t len = read((char*)buffer + offset, left);
        if(len <= 0){
            return len;
        }
        offset += len;
        left -= len;
    }
    return length;
}

int Stream::readExact(BxByteArray::ptr ba, size_t length){
    int64_t left = length;
    // 循环读取，把length长度的数据全部读出来
    while(left > 0){
        // 调用 this->read()，利用多态性进行读取
        int64_t len = read(ba, left);
        if(len <= 0){
            return len;
        }
        left -= len;
    }
    return length;
}

// 一直调子类 write() 直到写满 length,中途出错就提前返回。
// 写满返回 length,出错返回 <0(半包中途挂了不回报已写多少)。
// 协议层组装好响应后用它保证整块数据全送出去。
int Stream::writeExact(void* buffer, size_t length){
    size_t offset = 0;
    int64_t left = length;
    // 循环写入
    while(left > 0){
        // 调用 this->write()，利用多态性写入
        int64_t len = write((char*)buffer + offset, left);
        if(len <= 0){
            return len;
        }
        offset += len;
        left -= len;
    }
    return length;
}

int Stream::writeExact(BxByteArray::ptr ba, size_t length){
    int64_t left = length;
    // 循环写入，把length长度的数据全部读出来
    while(left > 0){
        // 调用 this->write()，利用多态性写入
        int64_t len = write(ba, left);
        if(len <= 0){
            return len;
        }
        left -= len;
    }
    return length;
}


}