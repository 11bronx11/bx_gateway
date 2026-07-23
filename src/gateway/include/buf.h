#pragma once

// 一条连接的读缓冲，对付 TCP 半包粘包。fill 从 socket 读进来攒着，
// findHeaderEnd 探 \r\n\r\n 头块边界，consume 丢掉已处理的。
// 双游标，不够就先左移回收再扩容，每连接单协程独占，不加锁。

#include <cstddef>
#include <cstring>
#include <string>

namespace bronx {

class BxSocket;  // 前向声明(fill 用)

namespace gateway {

class InBuf {
public:
    explicit InBuf(size_t initCap = 4096);

    // 可读字节数 / 可读区起始指针
    size_t readable() const { return m_wpos - m_rpos; }
    const char* peek() const { return m_data + m_rpos; }

    // 追加裸数据(测试/内部用)
    void append(const char* data, size_t len);

    // 从 socket 读一批追加进来。
    // 返回 >0 读入字节数,=0 对端关闭,<0 出错,errno 是 ETIMEDOUT ECANCELED 之类。
    // 连接循环里反复调,直到头块完整或者 body 读够。
    int fill(bronx::BxSocket* sock);

    // 消费(丢弃)前 n 字节(已解析/已转发的)。
    void consume(size_t n);

    // 消费全部
    void consumeAll() { m_rpos = m_wpos = 0; }

    // 尝试降低底层容量。会使旧的 peek() 返回指针失效。
    // min_capacity: 缩容后至少保留的容量。
    // reserve_writable: 除现有可读数据外至少预留的尾部可写空间。
    // 返回 true 表示发生了重新分配和缩容。
    bool trim(size_t min_capacity = 4096, size_t reserve_writable = 4096);

    // 找 \r\n\r\n 头块结束标记,找到返回头块长度(含这四字节),没找到返回 0。
    // 连接靠它判断头到齐没,齐了才喂解析器。
    size_t findHeaderEnd() const;

    // 当前缓冲容量(测试用)
    size_t capacity() const { return m_cap; }

    ~InBuf();
    InBuf(const InBuf&) = delete;
    InBuf& operator=(const InBuf&) = delete;

private:
    void ensure_writable(size_t need);

private:
    char*  m_data = nullptr;
    size_t m_cap  = 0;
    size_t m_rpos = 0;
    size_t m_wpos = 0;
};

} // namespace gateway
} // namespace bronx
