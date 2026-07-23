#pragma once

// 把 HTTP body 当成流来读，不整块进内存。按分帧方式从 InBuf 加 socket 边读边产 chunk，
// 内存只持一块，转发循环拿一块写一块，SSE 也能转。
// CL 读够长度结束，chunked 解到 0 长度块，until-close 读到对端关闭，单协程独占不加锁。

#include "buf.h"
#include "http_parse.h"   // BodyFraming
#include <string>
#include <memory>
#include <cstdint>
#include <functional>
#include <utility>

namespace bronx {
class BxSocket;
namespace gateway {

enum class BodyReadError {
    NONE,
    IO,
    MALFORMED,
    TOO_LARGE,
};

// body 编码(出方向):把一块 body 字节按目标分帧编码成可写字节。
// 用于响应回写:若上游 chunked,网关也 chunked 转发;若 CL 直接透传。
class BodyEncoder {
public:
    // 编码一块 body。chunked 模式包成 "<hexlen>\r\n<data>\r\n";CL/identity 模式原样返回。
    static std::string encodeChunk(BodyFraming framing, const char* data, size_t len);
    // chunked 收尾块 "0\r\n\r\n";非 chunked 返回空。
    static std::string encodeEnd(BodyFraming framing);
};

// 流式 body 读取器
class BodyReader {
public:
    using ptr = std::shared_ptr<BodyReader>;

    // framing:分帧方式;contentLength:CL 模式下的总长;
    // buf:头解析后持有的残留字节(body 起始);sock:继续读 body 的 socket。
    BodyReader(BodyFraming framing, uint64_t contentLength,
                   InBuf* buf, bronx::BxSocket* sock);

    // 读下一块解码后的 body 追加到 out。
    // 返回 >0 本次产出字节数,=0 正常读完,<0 出错超时或被取消。
    // 转发循环反复调,每块立刻写给对端。
    int readChunk(std::string& out);

    bool isFinished() const { return m_finished; }
    bool hasError() const { return m_error; }
    BodyReadError errorCode() const { return m_errorCode; }
    void setMaxBodySize(uint64_t maxBodySize) { m_maxBodySize = maxBodySize; }
    void setFillObserver(std::function<void(size_t)> cb) { m_onFill = std::move(cb); }

private:
    int read_clen(std::string& out);  // CL 定长
    int readChunked(std::string& out);        // chunked 解码
    int read_until_close(std::string& out);     // 读到关闭
    // 确保缓冲区里至少有 n 字节可读(不够则 fill);返回 false=对端关闭/错误
    bool ensure(size_t n);
    // 读一行(到 \r\n),用于 chunked 的长度行;返回 false=失败
    bool readLine(std::string& line);
    bool accountBytes(uint64_t n);
    void setError(BodyReadError code);
    void notifyFill();

private:
    BodyFraming  m_framing;
    uint64_t     m_remaining;     // CL 模式剩余字节
    InBuf* m_buf;
    bronx::BxSocket* m_sock;
    bool         m_finished = false;
    bool         m_error = false;
    BodyReadError m_errorCode = BodyReadError::NONE;
    uint64_t     m_totalRead = 0;
    uint64_t     m_maxBodySize = UINT64_MAX;
    std::function<void(size_t)> m_onFill;
    // chunked 状态
    bool         m_chunkDone = false;
    uint64_t     m_chunk_left = 0;
};

} // namespace gateway
} // namespace bronx
