#pragma once

// 装已解析的 HTTP 头，GwRequest 装请求头，GwResponse 装响应头，只装头不装 body。
// 解析器逐字段填进来，或者网关自己构造错误页短路响应。
// header 的 key 大小写不敏感。

#include <map>
#include <string>
#include <memory>
#include <cstdint>
#include <sstream>
#include <vector>

namespace bronx {
namespace gateway {

// HTTP 方法(网关关心的子集 + 常见;不认识的按 INVALID,但原文仍可转发)
enum class HttpMethod {
    DELETE_, GET, HEAD, POST, PUT, CONNECT, OPTIONS, TRACE, PATCH,
    INVALID_METHOD
};

// 方法名 <-> 枚举
HttpMethod        StringToHttpMethod(const std::string& m);
const char*       HttpMethodToString(HttpMethod m);

// HTTP 版本:用 uint8 高低位表示,0x11=HTTP/1.1, 0x10=HTTP/1.0
// （与旧 bronx 约定一致,便于心智迁移）

// 状态码 -> reason phrase（网关常用子集；缺失返回 "Unknown"）
const char* HttpStatusReason(int code);

// 大小写不敏感比较器（用于 header map 的 key 比较）
struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const;
};

using HeaderMap = std::map<std::string, std::string, CaseInsensitiveLess>;
using HeaderList = std::vector<std::pair<std::string, std::string>>;

// 请求头容器。dumpHead 产出请求行加头部发给上游。
class GwRequest {
public:
    using ptr = std::shared_ptr<GwRequest>;

    GwRequest() = default;

    // -- 请求行 --
    HttpMethod getMethod() const { return m_method; }
    void setMethod(HttpMethod v) { m_method = v; }
    const std::string& getMethodRaw() const { return m_methodRaw; }   // 原始方法串(透传不认识的方法)
    void setMethodRaw(const std::string& v) { m_methodRaw = v; }

    const std::string& getPath() const { return m_path; }
    void setPath(const std::string& v) { m_path = v; }

    const std::string& getQuery() const { return m_query; }
    void setQuery(const std::string& v) { m_query = v; }

    const std::string& getFragment() const { return m_fragment; }
    void setFragment(const std::string& v) { m_fragment = v; }

    uint8_t getVersion() const { return m_version; }
    void setVersion(uint8_t v) { m_version = v; }

    // -- 头部 --
    const HeaderMap& getHeaders() const { return m_headers; }
    HeaderMap& getHeaders() { return m_headers; }
    const HeaderList& getHeaderList() const { return m_headerList; }
    std::string getHeader(const std::string& key, const std::string& def = "") const;
    void setHeader(const std::string& key, const std::string& val);
    void addHeader(const std::string& key, const std::string& val);
    void delHeader(const std::string& key);
    bool hasHeader(const std::string& key) const { return m_headers.count(key) > 0; }

    // -- 连接控制(由 Connection 头解析得来,Layer 1 设置)--
    bool isClose() const { return m_close; }
    void setClose(bool v) { m_close = v; }
    bool isWebsocket() const { return m_websocket; }
    void setWebsocket(bool v) { m_websocket = v; }

    // 把请求行 + 头部 dump 成报文(不含 body)。用于发往上游。
    std::string dumpHead() const;

private:
    HttpMethod  m_method = HttpMethod::GET;
    std::string m_methodRaw;
    std::string m_path;
    std::string m_query;
    std::string m_fragment;
    uint8_t     m_version = 0x11;   // 默认 HTTP/1.1
    bool        m_close = false;    // 默认 keep-alive(HTTP/1.1 语义)
    bool        m_websocket = false;
    HeaderMap   m_headers;
    HeaderList  m_headerList;
};

// 响应头容器。dumpHead 产出状态行加头部回客户端。
class GwResponse {
public:
    using ptr = std::shared_ptr<GwResponse>;

    GwResponse() = default;

    int getStatus() const { return m_status; }
    void setStatus(int v) { m_status = v; }

    const std::string& getReason() const { return m_reason; }
    void setReason(const std::string& v) { m_reason = v; }

    uint8_t getVersion() const { return m_version; }
    void setVersion(uint8_t v) { m_version = v; }

    const HeaderMap& getHeaders() const { return m_headers; }
    HeaderMap& getHeaders() { return m_headers; }
    const HeaderList& getHeaderList() const { return m_headerList; }
    std::string getHeader(const std::string& key, const std::string& def = "") const;
    void setHeader(const std::string& key, const std::string& val);
    void addHeader(const std::string& key, const std::string& val);
    void delHeader(const std::string& key);
    bool hasHeader(const std::string& key) const { return m_headers.count(key) > 0; }

    bool isClose() const { return m_close; }
    void setClose(bool v) { m_close = v; }

    // 状态行 + 头部 dump(不含 body)。
    std::string dumpHead() const;

private:
    int         m_status = 200;
    std::string m_reason;           // 空则按 m_status 查表
    uint8_t     m_version = 0x11;
    bool        m_close = false;
    HeaderMap   m_headers;
    HeaderList  m_headerList;
};

} // namespace gateway
} // namespace bronx
