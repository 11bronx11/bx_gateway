#pragma once

// 网关里几个字符串和 header 文本小工具，纯函数不碰 socket 和 ctx。
// 原先散在好几个 cpp 里逐字节抄了好多份，收到一处省得各处飘。
// 注意 trim_ascii 只裁空白，http_parse 那个 trimOWS 更严还剥参数，是 TE 专用没并进来。

#include "http_msg.h"   // HeaderList
#include <string>
#include <cstddef>
#include <cstdint>
#include <strings.h>    // strcasecmp

namespace bronx {
namespace gateway {

// 裁掉首尾的 space/tab/CR/LF;全空白返回空串。
inline std::string trim_ascii(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if(b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 逗号分隔的 header 值里是否含某 token(大小写不敏感,逐段 trim 后比对)。
// 例:"gzip, chunked" 里找 "chunked"。空 token 直接 false。
inline bool hdr_has_token(const std::string& value, const std::string& token) {
    if(token.empty()) return false;
    size_t pos = 0;
    while(pos <= value.size()) {
        size_t comma = value.find(',', pos);
        std::string part = trim_ascii(value.substr(pos, comma == std::string::npos
                                                     ? std::string::npos : comma - pos));
        if(strcasecmp(part.c_str(), token.c_str()) == 0) {
            return true;
        }
        if(comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

// 把同名(大小写不敏感)header 的多个值用逗号拼成一条。无匹配返回空串。
inline std::string join_hdr_vals(const HeaderList& headers, const std::string& key) {
    std::string out;
    for(const auto& h : headers) {
        if(strcasecmp(h.first.c_str(), key.c_str()) != 0) {
            continue;
        }
        if(!out.empty()) {
            out += ",";
        }
        out += h.second;
    }
    return out;
}

inline std::string host_port(const std::string& host, uint32_t port) {
    if(host.find(':') != std::string::npos && host.front() != '[') {
        return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
}

} // namespace gateway
} // namespace bronx
