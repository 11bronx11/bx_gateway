// 请求头解析器 fuzz 目标。喂任意字节给 HttpReqParser 那套 Mongrel 状态机,
// 挂 ASan/UBSan 抓崩溃越界 UB。重点盯请求走私:头解析完调 getBodyFraming
// 看 CL/TE 冲突判定路径。独立编译不碰框架 hook/协程。
//
// 契约(见 http_parse.cpp:129 注释):Mongrel 是【非增量】解析器。真实路径是
// conn.cpp 先靠 InBuf::findHeaderEnd 攒到完整 \r\n\r\n 头块,再【新建】parser
// 一次性 off=0 喂 headLen 字节。所以本 harness 必须复刻:找头块边界 → 一次喂。
// 不能把同一 parser 半包重入(那样 mark/query_start 残留偏移会假崩,非真实可达)。
#include "http_parse.h"
#include <cstddef>
#include <cstdint>
#include <string>

using bronx::gateway::HttpReqParser;

// 复刻 InBuf::findHeaderEnd 语义:找 \r\n\r\n,返回含这四字节的头块长度,没有返回 0。
// 不直接用 InBuf 是因为 buf.cpp 会拖进 GwPool 内存池 + net_socket,污染 fuzz 独立性。
// 逻辑就一行扫描,自己写更干净。
static size_t find_header_end(const char* p, size_t len) {
    for(size_t i = 0; i + 3 < len; ++i) {
        if(p[i] == '\r' && p[i+1] == '\n' && p[i+2] == '\r' && p[i+3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

static void run_one_request(const uint8_t* data, size_t size) {
    const char* buf = reinterpret_cast<const char*>(data);
    size_t headLen = find_header_end(buf, size);
    if(headLen == 0) return;   // 头块不完整 真实路径会继续 fill 等待 这里没得等

    // conn.cpp:197-198:每条请求新建 parser 一次性喂完整头块
    HttpReqParser p;
    size_t used = p.execute(buf, headLen);
    (void)used;

    if(p.hasError() || !p.isFinished()) return;

    // 走私判定路径 内部查 CL/TE 冲突 多 CL 不一致
    auto framing = p.getBodyFraming();
    volatile uint64_t cl = p.getContentLength();
    (void)framing; (void)cl;

    auto req = p.getData();
    if(req) {
        volatile size_t n = req->getHeaders().size();
        (void)n;
        (void)req->getHeader("transfer-encoding");
        (void)req->getHeader("content-length");
        (void)req->getPath();
        (void)req->getQuery();
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if(size == 0) return 0;
    run_one_request(data, size);
    return 0;
}
