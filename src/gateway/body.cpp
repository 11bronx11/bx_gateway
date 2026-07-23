#include "body.h"
#include "net_socket.h"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <climits>
#include <cctype>
#include <limits>

namespace bronx {
namespace gateway {

// BodyEncoder
std::string BodyEncoder::encodeChunk(BodyFraming framing, const char* data, size_t len) {
    if(framing == BodyFraming::CHUNKED) {
        char hdr[32];
        int n = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
        std::string out;
        out.reserve(n + len + 2);
        out.append(hdr, n);
        out.append(data, len);
        out.append("\r\n", 2);
        return out;
    }
    // CL / identity:原样
    return std::string(data, len);
}

std::string BodyEncoder::encodeEnd(BodyFraming framing) {
    if(framing == BodyFraming::CHUNKED) {
        return "0\r\n\r\n";
    }
    return std::string();
}

// BodyReader
BodyReader::BodyReader(BodyFraming framing, uint64_t contentLength,
                               InBuf* buf, bronx::BxSocket* sock)
    : m_framing(framing)
    , m_remaining(contentLength)
    , m_buf(buf)
    , m_sock(sock) {
    // NONE:无 body,直接完成
    if(framing == BodyFraming::NONE ||
       (framing == BodyFraming::CONTENT_LENGTH && contentLength == 0)) {
        m_finished = true;
    }
}

void BodyReader::notifyFill() {
    if(m_onFill && m_buf) {
        m_onFill(m_buf->readable());
    }
}

// 确保缓冲至少 n 字节可读;不够则 fill。返回 false=对端关闭/错误
bool BodyReader::ensure(size_t n) {
    while(m_buf->readable() < n) {
        if(!m_sock) {
            setError(BodyReadError::IO);
            return false;
        }
        int rt = m_buf->fill(m_sock);
        if(rt <= 0) {
            setError(BodyReadError::IO);
            return false;
        }
        notifyFill();
    }
    return true;
}

// 读一行(到 \r\n),消费掉含 \r\n;line 不含 \r\n。返回 false=失败
bool BodyReader::readLine(std::string& line) {
    // 在缓冲里找 \r\n,不够就 fill
    while(true) {
        const char* p = m_buf->peek();
        size_t len = m_buf->readable();
        for(size_t i = 0; i + 1 < len; ++i) {
            if(p[i] == '\r' && p[i+1] == '\n') {
                line.assign(p, i);
                m_buf->consume(i + 2);
                return true;
            }
        }
        // 防超长 chunk 长度行(攻击)
        if(len > 8192) {
            setError(BodyReadError::MALFORMED);
            return false;
        }
        if(!m_sock) {
            setError(BodyReadError::IO);
            return false;
        }
        int rt = m_buf->fill(m_sock);
        if(rt <= 0) {
            setError(BodyReadError::IO);
            return false;
        }
        notifyFill();
    }
}

void BodyReader::setError(BodyReadError code) {
    m_error = true;
    if(m_errorCode == BodyReadError::NONE) {
        m_errorCode = code;
    }
}

bool BodyReader::accountBytes(uint64_t n) {
    if(n > m_maxBodySize || m_totalRead > m_maxBodySize - n) {
        setError(BodyReadError::TOO_LARGE);
        m_finished = true;
        return false;
    }
    m_totalRead += n;
    return true;
}

int BodyReader::readChunk(std::string& out) {
    if(m_finished) return 0;
    if(m_error)    return -1;
    switch(m_framing) {
        case BodyFraming::CONTENT_LENGTH: return read_clen(out);
        case BodyFraming::CHUNKED:        return readChunked(out);
        case BodyFraming::UNTIL_CLOSE:    return read_until_close(out);
        default:                          m_finished = true; return 0;
    }
}

// <!-- PLACEHOLDER_READERS -->

// CL 定长:读 min(remaining, 缓冲可读 或 新读一批),产出一块
int BodyReader::read_clen(std::string& out) {
    if(m_remaining == 0) {
        m_finished = true;
        return 0;
    }
    // 缓冲里没有就先读一批
    if(m_buf->readable() == 0) {
        if(!m_sock) {
            setError(BodyReadError::IO);
            m_finished = true;
            return -1;
        }
        int rt = m_buf->fill(m_sock);
        if(rt <= 0) {
            // body 未读够却关闭/出错
            setError(BodyReadError::IO);
            m_finished = true;
            return -1;
        }
        notifyFill();
    }
    size_t take = m_buf->readable();
    if(take > m_remaining) take = m_remaining;
    if(!accountBytes(take)) {
        return -1;
    }
    out.append(m_buf->peek(), take);
    m_buf->consume(take);
    m_remaining -= take;
    if(m_remaining == 0) {
        m_finished = true;
    }
    return (int)take;
}

// chunked 解码:读一帧 <hexlen>\r\n<data>\r\n,0 长度帧结束
int BodyReader::readChunked(std::string& out) {
    if(m_chunk_left == 0) {
        // 读长度行
        std::string lenLine;
        if(!readLine(lenLine)) {
            m_finished = true;
            return m_error ? -1 : 0;
        }
        size_t semi = lenLine.find(';');
        if(semi != std::string::npos) lenLine.resize(semi);
        if(lenLine.empty()) {
            setError(BodyReadError::MALFORMED);
            return -1;
        }
        for(unsigned char c : lenLine) {
            if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                 || (c >= 'A' && c <= 'F'))) {
                setError(BodyReadError::MALFORMED);
                return -1;
            }
        }
        char* endp = nullptr;
        errno = 0;
        unsigned long long clen = strtoull(lenLine.c_str(), &endp, 16);
        if(endp == lenLine.c_str() || *endp != '\0' || errno == ERANGE) {
            setError(BodyReadError::MALFORMED);
            return -1;   // 非法长度
        }
        if(clen == 0) {
            // 末块:读掉结尾 \r\n(可能有 trailer,这里安全忽略到空行)。
            // trailer 行数设上限:单行已有 8192 上限,但行数无限时对端可在 0\r\n 后
            // 持续发 "X:y\r\n" 无限占用本连接协程(内存不涨但 CPU/连接耗)。
            static const int kMaxTrailerLines = 32;
            std::string trailer;
            int trailerLines = 0;
            while(readLine(trailer) && !trailer.empty()) {
                if(++trailerLines > kMaxTrailerLines) {
                    setError(BodyReadError::MALFORMED);
                    m_finished = true;
                    return -1;
                }
            }
            m_finished = true;
            return 0;
        }
        if(!accountBytes(clen)) {
            return -1;
        }
        m_chunk_left = clen;
    }

    if(m_buf->readable() == 0) {
        if(!m_sock) {
            setError(BodyReadError::IO);
            m_finished = true;
            return -1;
        }
        int rt = m_buf->fill(m_sock);
        if(rt <= 0) {
            setError(BodyReadError::IO);
            m_finished = true;
            return -1;
        }
        notifyFill();
    }
    size_t take = m_buf->readable();
    if(take > m_chunk_left) {
        take = (size_t)m_chunk_left;
    }
    out.append(m_buf->peek(), take);
    m_buf->consume(take);
    m_chunk_left -= take;

    if(m_chunk_left == 0) {
        // 读掉数据后的 \r\n
        if(!ensure(2)) {
            m_finished = true;
            if(!m_error) setError(BodyReadError::IO);
            return -1;
        }
        if(m_buf->peek()[0] != '\r' || m_buf->peek()[1] != '\n') {
            setError(BodyReadError::MALFORMED);
            m_finished = true;
            return -1;
        }
        m_buf->consume(2);
    }
    return (int)take;
}

// until-close:一直读到对端关闭(响应无 CL/无 chunked 时用)
int BodyReader::read_until_close(std::string& out) {
    if(m_buf->readable() == 0) {
        if(!m_sock) {
            m_finished = true;
            return 0;
        }
        int rt = m_buf->fill(m_sock);
        if(rt <= 0) {
            if(rt < 0) {
                setError(BodyReadError::IO);
            }
            m_finished = true;
            return rt < 0 ? -1 : 0;
        }
        notifyFill();
    }
    size_t take = m_buf->readable();
    if(!accountBytes(take)) {
        return -1;
    }
    out.append(m_buf->peek(), take);
    m_buf->consume(take);
    return (int)take;
}


} // namespace gateway
} // namespace bronx
