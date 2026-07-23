#include "http_msg.h"
#include "str.h"
#include <strings.h>   // strcasecmp
#include <algorithm>

namespace bronx {
namespace gateway {

// 方法名映射
HttpMethod StringToHttpMethod(const std::string& m) {
    if(m == "GET")     return HttpMethod::GET;
    if(m == "POST")    return HttpMethod::POST;
    if(m == "HEAD")    return HttpMethod::HEAD;
    if(m == "PUT")     return HttpMethod::PUT;
    if(m == "DELETE")  return HttpMethod::DELETE_;
    if(m == "OPTIONS") return HttpMethod::OPTIONS;
    if(m == "CONNECT") return HttpMethod::CONNECT;
    if(m == "TRACE")   return HttpMethod::TRACE;
    if(m == "PATCH")   return HttpMethod::PATCH;
    return HttpMethod::INVALID_METHOD;
}

const char* HttpMethodToString(HttpMethod m) {
    switch(m) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE_: return "DELETE";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::CONNECT: return "CONNECT";
        case HttpMethod::TRACE:   return "TRACE";
        case HttpMethod::PATCH:   return "PATCH";
        default:                  return "INVALID";
    }
}

// 状态码 reason（网关常用子集）
const char* HttpStatusReason(int code) {
    switch(code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 417: return "Expectation Failed";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
}

// 版本号 uint8(0x11) -> "HTTP/1.1"
static std::string ver_to_str(uint8_t v) {
    return std::string("HTTP/") + std::to_string(v >> 4) + "." + std::to_string(v & 0x0F);
}

static void erase_hdr(HeaderList& headers, const std::string& key) {
    headers.erase(std::remove_if(headers.begin(), headers.end(),
        [&](const auto& h) {
            return strcasecmp(h.first.c_str(), key.c_str()) == 0;
        }), headers.end());
}

// GwRequest
std::string GwRequest::getHeader(const std::string& key, const std::string& def) const {
    auto it = m_headers.find(key);
    return it == m_headers.end() ? def : it->second;
}

void GwRequest::setHeader(const std::string& key, const std::string& val) {
    m_headers[key] = val;
    erase_hdr(m_headerList, key);
    m_headerList.emplace_back(key, val);
}

void GwRequest::addHeader(const std::string& key, const std::string& val) {
    auto it = m_headers.find(key);
    if(strcasecmp(key.c_str(), "X-Forwarded-For") == 0 && it != m_headers.end()) {
        it->second += ", " + val;
    } else {
        m_headers[key] = val;
    }
    m_headerList.emplace_back(key, val);
}

void GwRequest::delHeader(const std::string& key) {
    m_headers.erase(key);
    erase_hdr(m_headerList, key);
}

std::string GwRequest::dumpHead() const {
    std::stringstream ss;
    // 请求行:METHOD SP path[?query] SP HTTP/x.y CRLF
    const char* method = m_methodRaw.empty() ? HttpMethodToString(m_method) : m_methodRaw.c_str();
    ss << method << " " << m_path;
    if(!m_query.empty()) ss << "?" << m_query;
    ss << " " << ver_to_str(m_version) << "\r\n";
    if(!m_headerList.empty()) {
        for(const auto& h : m_headerList) {
            ss << h.first << ": " << h.second << "\r\n";
        }
    } else {
        for(auto& h : m_headers) {
            ss << h.first << ": " << h.second << "\r\n";
        }
    }
    ss << "\r\n";
    return ss.str();
}

// GwResponse
std::string GwResponse::getHeader(const std::string& key, const std::string& def) const {
    auto it = m_headers.find(key);
    return it == m_headers.end() ? def : it->second;
}

void GwResponse::setHeader(const std::string& key, const std::string& val) {
    if(strcasecmp(key.c_str(), "Vary") == 0) {
        auto it = m_headers.find("Vary");
        if(it != m_headers.end() && !trim_ascii(it->second).empty()) {
            if(!hdr_has_token(it->second, val)) {
                it->second += ", ";
                it->second += val;
                erase_hdr(m_headerList, key);
                m_headerList.emplace_back(it->first, it->second);
            }
            return;
        }
    }
    m_headers[key] = val;
    erase_hdr(m_headerList, key);
    m_headerList.emplace_back(key, val);
}

void GwResponse::addHeader(const std::string& key, const std::string& val) {
    m_headers[key] = val;
    m_headerList.emplace_back(key, val);
}

void GwResponse::delHeader(const std::string& key) {
    m_headers.erase(key);
    erase_hdr(m_headerList, key);
}

std::string GwResponse::dumpHead() const {
    std::stringstream ss;
    const std::string reason = m_reason.empty() ? HttpStatusReason(m_status) : m_reason;
    // 状态行:HTTP/x.y SP code SP reason CRLF
    ss << ver_to_str(m_version) << " " << m_status << " " << reason << "\r\n";
    if(!m_headerList.empty()) {
        for(const auto& h : m_headerList) {
            ss << h.first << ": " << h.second << "\r\n";
        }
    } else {
        for(auto& h : m_headers) {
            ss << h.first << ": " << h.second << "\r\n";
        }
    }
    ss << "\r\n";
    return ss.str();
}

} // namespace gateway
} // namespace bronx
