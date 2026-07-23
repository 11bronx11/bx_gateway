#pragma once

// 把 gw_parser 那套 Mongrel 状态机包一层，喂字节出头对象。
// 请求头解析成 GwRequest，纯字节驱动不碰 socket 方便单测，顺带定出 body 分帧方式，
// 判定时做请求走私防御。响应头解析器在下面另有一个。

#include "http_msg.h"
#include <memory>

struct http_parser;  // Mongrel C struct(前向声明,避免污染头)
struct httpclient_parser;

namespace bronx {
namespace gateway {

// body 分帧方式(HTTP 消息体边界如何确定)
enum class BodyFraming {
    NONE,           // 无 body(无 CL、无 TE,如 GET)
    CONTENT_LENGTH, // 由 Content-Length 定长
    CHUNKED,        // Transfer-Encoding: chunked
    UNTIL_CLOSE,    // 读到连接关闭为止(响应才会用,请求一般不用)
};

class HttpReqParser {
public:
    using ptr = std::shared_ptr<HttpReqParser>;

    HttpReqParser();
    ~HttpReqParser();

    // 喂字节解析。返回本次"消费(已解析)"的字节数。
    // 输入:data/len = 当前缓冲区里的全部未解析字节;输出:消费字节数。
    // 半包安全:未解析完返回部分消费,调用方保留剩余下次再喂。
    size_t execute(const char* data, size_t len);

    bool isFinished() const;   // 头部是否解析完成
    bool hasError() const;     // 是否解析出错(含走私拒绝)

    GwRequest::ptr getData() const { return m_data; }

    // 头解析完成后,确定 body 分帧方式(含走私检测)。
    // 走私防御:Content-Length 与 Transfer-Encoding 同时出现、或出现多个不一致 CL → 置错误。
    BodyFraming getBodyFraming();
    uint64_t getContentLength() const { return m_clen; }

    // 头部缓冲上限(超过即 431,防超长 header 攻击)
    static uint64_t MaxHeaderSize();

private:
    // Mongrel 回调(C 风格,通过 data 指针回到本对象)
    static void onRequestMethod(void* data, const char* at, size_t len);
    static void onRequestURI(void* data, const char* at, size_t len);
    static void onRequestPath(void* data, const char* at, size_t len);
    static void onQueryString(void* data, const char* at, size_t len);
    static void onFragment(void* data, const char* at, size_t len);
    static void onHttpVersion(void* data, const char* at, size_t len);
    static void onHeaderDone(void* data, const char* at, size_t len);
    static void onHttpField(void* data, const char* field, size_t flen,
                            const char* value, size_t vlen);

private:
    http_parser*   m_parser;        // Mongrel 解析器(堆分配,析构释放)
    GwRequest::ptr m_data;
    int            m_error = 0;      // 0=ok;1000 method;1001 version;1002 field;1003 走私
    uint64_t       m_clen = 0;
    bool           m_has_clen = false;
    bool           m_hasChunked = false;
};

// 上游响应头解析，包的是 Mongrel 的 httpclient_parser，产出 GwResponse 加分帧信息。
// Mongrel 非增量，得攒够完整头块 \r\n\r\n 再一次 execute。
class HttpRespParser {
public:
    using ptr = std::shared_ptr<HttpRespParser>;

    HttpRespParser();
    ~HttpRespParser();

    size_t execute(const char* data, size_t len);
    bool isFinished() const;
    bool hasError() const;

    GwResponse::ptr getData() const { return m_data; }

    // 响应 body 分帧:chunked > content-length > until-close(响应特有)。
    BodyFraming getBodyFraming() const;
    uint64_t getContentLength() const { return m_clen; }

private:
    static void onReasonPhrase(void* data, const char* at, size_t len);
    static void onStatusCode(void* data, const char* at, size_t len);
    static void onHttpVersion(void* data, const char* at, size_t len);
    static void onHeaderDone(void* data, const char* at, size_t len);
    static void onHttpField(void* data, const char* field, size_t flen,
                            const char* value, size_t vlen);

private:
    httpclient_parser* m_parser;
    GwResponse::ptr    m_data;
    int                m_error = 0;
    uint64_t           m_clen = 0;
    bool               m_has_clen = false;
    bool               m_hasChunked = false;
};

} // namespace gateway
} // namespace bronx
