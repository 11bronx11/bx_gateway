#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // for strcasestr
#endif
#include <string.h>
#include "http_parse.h"
#include "gw_parser/http11_parser.h"
#include "gw_parser/httpclient_parser.h"
#include <strings.h>
#include <cstdlib>
#include <cerrno>
#include <cctype>

namespace bronx {
namespace gateway {

static std::string trimOWS(const std::string& v) {
    size_t begin = 0;
    while(begin < v.size() && std::isspace((unsigned char)v[begin])) {
        ++begin;
    }
    size_t end = v.size();
    while(end > begin && std::isspace((unsigned char)v[end - 1])) {
        --end;
    }
    return v.substr(begin, end - begin);
}

// Content-Length 必须是纯十进制数字串。strtoull 会静默接受前导 '-'(取负回绕成
// 巨大 uint64)、'+'、前导空白,构成走私/超大 body 向量,故先卡死格式再解析。
static bool all_digits(const std::string& s) {
    if(s.empty()) return false;
    for(unsigned char c : s) {
        if(c < '0' || c > '9') return false;
    }
    return true;
}

static bool hdr_has_token(const std::string& value, const char* token) {
    size_t pos = 0;
    while(pos <= value.size()) {
        size_t comma = value.find(',', pos);
        std::string part = value.substr(pos, comma == std::string::npos
                                             ? std::string::npos : comma - pos);
        size_t semi = part.find(';');
        if(semi != std::string::npos) {
            part.resize(semi);
        }
        part = trimOWS(part);
        if(strcasecmp(part.c_str(), token) == 0) {
            return true;
        }
        if(comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

static bool parse_te(const std::string& value,
                                  bool* hasChunked,
                                  bool* unsupported,
                                  bool* malformed) {
    *hasChunked = false;
    *unsupported = false;
    *malformed = false;
    size_t pos = 0;
    bool sawToken = false;
    while(pos <= value.size()) {
        size_t comma = value.find(',', pos);
        std::string part = value.substr(pos, comma == std::string::npos
                                             ? std::string::npos : comma - pos);
        size_t semi = part.find(';');
        if(semi != std::string::npos) {
            part.resize(semi);
        }
        part = trimOWS(part);
        if(part.empty()) {
            *malformed = true;
            return false;
        }
        sawToken = true;
        bool isLast = comma == std::string::npos;
        if(strcasecmp(part.c_str(), "chunked") == 0) {
            *hasChunked = true;
            if(!isLast) {
                *malformed = true;
                return false;
            }
        } else {
            *unsupported = true;
        }
        if(comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    if(!sawToken) {
        *malformed = true;
        return false;
    }
    return true;
}

// 头部缓冲上限默认 8K（防超长 header 攻击;超过 → 431）
uint64_t HttpReqParser::MaxHeaderSize() {
    return 8 * 1024;
}

HttpReqParser::HttpReqParser()
    : m_parser(new http_parser())
    , m_data(std::make_shared<GwRequest>()) {
    http_parser_init(m_parser);
    m_parser->data = this;               // 回调里取回本对象
    m_parser->request_method = onRequestMethod;
    m_parser->request_uri    = onRequestURI;
    m_parser->request_path   = onRequestPath;
    m_parser->query_string   = onQueryString;
    m_parser->fragment       = onFragment;
    m_parser->http_version   = onHttpVersion;
    m_parser->header_done    = onHeaderDone;
    m_parser->http_field     = onHttpField;
}

HttpReqParser::~HttpReqParser() {
    delete m_parser;
}

// 单次解析完整头块。Mongrel 是【非增量】解析器:必须一次性看到从 off=0 起的完整
// 头块(以 \r\n\r\n 结尾)。半包累积/边界探测由 Layer 2 的 InBuf 负责——
// 它检测到 \r\n\r\n 后,把完整头块一次性交给本函数(off=0)。
// 输入:data/len = 完整(或含尾随 body 起始的)头块;输出:消费(头部)字节数。
size_t HttpReqParser::execute(const char* data, size_t len) {
    size_t nparse = http_parser_execute(m_parser, data, len, 0);
    return nparse;
}

bool HttpReqParser::isFinished() const {
    return http_parser_is_finished(m_parser);
}

bool HttpReqParser::hasError() const {
    return m_error != 0 || http_parser_has_error(m_parser);
}

// 确定 body 分帧 + 走私防御
BodyFraming HttpReqParser::getBodyFraming() {
    // 走私防御:CL 与 chunked 同时出现 → 歧义,拒绝(请求走私经典向量)
    if(m_has_clen && m_hasChunked) {
        m_error = 1003;
        return BodyFraming::NONE;
    }
    if(m_hasChunked) {
        return BodyFraming::CHUNKED;
    }
    if(m_has_clen) {
        return m_clen > 0 ? BodyFraming::CONTENT_LENGTH : BodyFraming::NONE;
    }
    return BodyFraming::NONE;
}

// Mongrel 回调实现
#define SELF(d) static_cast<HttpReqParser*>(d)

void HttpReqParser::onRequestMethod(void* d, const char* at, size_t len) {
    auto self = SELF(d);
    std::string m(at, len);
    self->m_data->setMethodRaw(m);
    HttpMethod hm = StringToHttpMethod(m);
    self->m_data->setMethod(hm);
    // 不认识的方法不报错(透明转发),仅标 INVALID_METHOD
}

void HttpReqParser::onRequestURI(void*, const char*, size_t) {
    // 完整 URI(含 path/query/fragment),Mongrel 会再拆出 path/query/fragment,这里不用单独存
}

void HttpReqParser::onRequestPath(void* d, const char* at, size_t len) {
    SELF(d)->m_data->setPath(std::string(at, len));
}

void HttpReqParser::onQueryString(void* d, const char* at, size_t len) {
    SELF(d)->m_data->setQuery(std::string(at, len));
}

void HttpReqParser::onFragment(void* d, const char* at, size_t len) {
    SELF(d)->m_data->setFragment(std::string(at, len));
}

void HttpReqParser::onHttpVersion(void* d, const char* at, size_t len) {
    auto self = SELF(d);
    // "HTTP/1.1" / "HTTP/1.0"
    std::string v(at, len);
    uint8_t ver = 0x11;
    if(v == "HTTP/1.0") ver = 0x10;
    else if(v == "HTTP/1.1") ver = 0x11;
    else self->m_error = 1001;   // 不支持的版本
    self->m_data->setVersion(ver);
}

void HttpReqParser::onHeaderDone(void*, const char*, size_t) {
    // 头部结束标记(此处无需额外处理;分帧在 getBodyFraming 统一判定)
}

void HttpReqParser::onHttpField(void* d, const char* field, size_t flen,
                                        const char* value, size_t vlen) {
    auto self = SELF(d);
    std::string key(field, flen);
    std::string val(value, vlen);

    // Content-Length:记录并检测重复且不一致(走私向量)
    if(strcasecmp(key.c_str(), "Content-Length") == 0) {
        std::string clValue = trimOWS(val);
        char* endp = nullptr;
        errno = 0;
        uint64_t cl = strtoull(clValue.c_str(), &endp, 10);
        if(!all_digits(clValue) || endp == clValue.c_str() || *endp != '\0' || errno == ERANGE) {
            self->m_error = 1003;   // 非法 CL
        } else if(self->m_has_clen && self->m_clen != cl) {
            self->m_error = 1003;   // 重复且不一致 CL → 拒
        } else {
            self->m_clen = cl;
            self->m_has_clen = true;
        }
    }
    // Transfer-Encoding: chunked
    else if(strcasecmp(key.c_str(), "Transfer-Encoding") == 0) {
        bool hasChunked = false;
        bool unsupported = false;
        bool malformed = false;
        parse_te(val, &hasChunked, &unsupported, &malformed);
        if(malformed || unsupported) {
            self->m_error = 1003;
        } else if(hasChunked) {
            self->m_hasChunked = true;
        }
    }
    // Connection: 控制 keep-alive
    else if(strcasecmp(key.c_str(), "Connection") == 0) {
        if(hdr_has_token(val, "close")) {
            self->m_data->setClose(true);
        } else if(hdr_has_token(val, "keep-alive")) {
            self->m_data->setClose(false);
        } else if(hdr_has_token(val, "upgrade")) {
            // 可能是 WebSocket 升级,标记待 Upgrade 头确认
        }
    }
    // Upgrade: websocket
    else if(strcasecmp(key.c_str(), "Upgrade") == 0) {
        if(hdr_has_token(val, "websocket")) {
            self->m_data->setWebsocket(true);
        }
    }

    self->m_data->addHeader(key, val);
}

#undef SELF

// HttpRespParser(封装 httpclient_parser)

HttpRespParser::HttpRespParser()
    : m_parser(new httpclient_parser())
    , m_data(std::make_shared<GwResponse>()) {
    httpclient_parser_init(m_parser);
    m_parser->data = this;
    m_parser->reason_phrase = onReasonPhrase;
    m_parser->status_code   = onStatusCode;
    m_parser->http_version  = onHttpVersion;
    m_parser->header_done   = onHeaderDone;
    m_parser->http_field    = onHttpField;
}

HttpRespParser::~HttpRespParser() {
    delete m_parser;
}

size_t HttpRespParser::execute(const char* data, size_t len) {
    return httpclient_parser_execute(m_parser, data, len, 0);
}

bool HttpRespParser::isFinished() const {
    return httpclient_parser_is_finished(m_parser);
}

bool HttpRespParser::hasError() const {
    return m_error != 0 || httpclient_parser_has_error(m_parser);
}

// 响应 body 分帧:chunked 优先,其次 CL,否则 until-close(响应特有,无 CL/chunked 时
// 读到连接关闭为止)。注意:204/304 与 HEAD 响应无 body,由调用方按状态码额外判定。
BodyFraming HttpRespParser::getBodyFraming() const {
    if(m_hasChunked) return BodyFraming::CHUNKED;
    if(m_has_clen) {
        return m_clen > 0 ? BodyFraming::CONTENT_LENGTH : BodyFraming::NONE;
    }
    return BodyFraming::UNTIL_CLOSE;
}

#define RSELF(d) static_cast<HttpRespParser*>(d)

void HttpRespParser::onReasonPhrase(void* d, const char* at, size_t len) {
    RSELF(d)->m_data->setReason(std::string(at, len));
}

void HttpRespParser::onStatusCode(void* d, const char* at, size_t len) {
    RSELF(d)->m_data->setStatus(atoi(std::string(at, len).c_str()));
}

void HttpRespParser::onHttpVersion(void* d, const char* at, size_t len) {
    std::string v(at, len);
    RSELF(d)->m_data->setVersion(v == "HTTP/1.0" ? 0x10 : 0x11);
}

void HttpRespParser::onHeaderDone(void*, const char*, size_t) {}

void HttpRespParser::onHttpField(void* d, const char* field, size_t flen,
                                         const char* value, size_t vlen) {
    auto self = RSELF(d);
    std::string key(field, flen);
    std::string val(value, vlen);
    if(strcasecmp(key.c_str(), "Content-Length") == 0) {
        std::string clValue = trimOWS(val);
        char* endp = nullptr;
        errno = 0;
        uint64_t cl = strtoull(clValue.c_str(), &endp, 10);
        if(!all_digits(clValue) || endp == clValue.c_str() || *endp != '\0' || errno == ERANGE) {
            self->m_error = 1003;
        } else if(self->m_has_clen && self->m_clen != cl) {
            self->m_error = 1003;
        } else if(self->m_hasChunked) {
            self->m_error = 1003;
        } else {
            self->m_clen = cl;
            self->m_has_clen = true;
        }
    } else if(strcasecmp(key.c_str(), "Transfer-Encoding") == 0) {
        bool hasChunked = false;
        bool unsupported = false;
        bool malformed = false;
        parse_te(val, &hasChunked, &unsupported, &malformed);
        if(malformed || unsupported) {
            self->m_error = 1003;
        } else if(hasChunked) {
            if(self->m_has_clen) {
                self->m_error = 1003;
            }
            self->m_hasChunked = true;
        }
    } else if(strcasecmp(key.c_str(), "Connection") == 0) {
        if(hdr_has_token(val, "close")) self->m_data->setClose(true);
    }
    self->m_data->addHeader(key, val);
}

#undef RSELF

} // namespace gateway
} // namespace bronx
