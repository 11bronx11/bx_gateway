// G0 回归:Mongrel 解析器封装(HttpReqParser)正确性
// =========================================================
// 复用 bronx 的 Mongrel 状态机前必须自证可靠:正常请求、半包喂入、分帧识别、
// 走私拒绝(CL+TE / 重复不一致 CL)、不认识的方法透传。

#include "test_util.h"
#include "http_parse.h"
#include <string>
#include <strings.h>

using namespace bronx::gateway;

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

// 把整段报文一次喂入,返回解析器
static HttpReqParser::ptr parse_all(const std::string& raw) {
    auto p = std::make_shared<HttpReqParser>();
    p->execute(raw.data(), raw.size());
    return p;
}

static HttpRespParser::ptr parse_rsp(const std::string& raw) {
    auto p = std::make_shared<HttpRespParser>();
    p->execute(raw.data(), raw.size());
    return p;
}

// 1. 正常 GET:方法/路径/版本/头部
static void test_basic_get() {
    std::string raw =
        "GET /api/v1/users?id=42 HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: test\r\n"
        "\r\n";
    auto p = parse_all(raw);
    TEST_CHECK_MSG(p->isFinished(), "should finish");
    TEST_CHECK_MSG(!p->hasError(), "should have no error");
    auto req = p->getData();
    TEST_CHECK_MSG(req->getMethod() == HttpMethod::GET, "method GET");
    TEST_CHECK_MSG(req->getPath() == "/api/v1/users", "path got: " << req->getPath());
    TEST_CHECK_MSG(req->getQuery() == "id=42", "query got: " << req->getQuery());
    TEST_CHECK_MSG(req->getVersion() == 0x11, "version 1.1");
    TEST_CHECK_MSG(req->getHeader("host") == "example.com", "host header (case-insensitive)");
    TEST_CHECK_MSG(p->getBodyFraming() == BodyFraming::NONE, "GET no body");
}

// 2. Content-Length body 分帧
static void test_content_length() {
    std::string raw =
        "POST /submit HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 11\r\n"
        "\r\n";
    auto p = parse_all(raw);
    TEST_CHECK_MSG(p->isFinished(), "finish");
    TEST_CHECK_MSG(p->getBodyFraming() == BodyFraming::CONTENT_LENGTH, "framing=CL");
    TEST_CHECK_EQ((int)p->getContentLength(), 11);
}

// 3. chunked 分帧
static void test_chunked() {
    std::string raw =
        "POST /stream HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    auto p = parse_all(raw);
    TEST_CHECK_MSG(p->isFinished(), "finish");
    TEST_CHECK_MSG(p->getBodyFraming() == BodyFraming::CHUNKED, "framing=chunked");
}

static void test_transfer_encoding_token_boundary() {
    {
        std::string raw =
            "POST /stream HTTP/1.1\r\n"
            "Host: x\r\n"
            "Transfer-Encoding: chunked;foo=bar\r\n"
            "\r\n";
        auto p = parse_all(raw);
        TEST_CHECK_MSG(p->getBodyFraming() == BodyFraming::CHUNKED,
                       "chunked with extension recognized");
    }
    {
        std::string raw =
            "POST /stream HTTP/1.1\r\n"
            "Host: x\r\n"
            "Transfer-Encoding: notchunked\r\n"
            "\r\n";
        auto p = parse_all(raw);
        TEST_CHECK_MSG(p->hasError(),
                       "unsupported transfer-coding must be rejected");
    }
    {
        std::string raw =
            "POST /stream HTTP/1.1\r\n"
            "Host: x\r\n"
            "Transfer-Encoding: gzip, chunked\r\n"
            "\r\n";
        auto p = parse_all(raw);
        TEST_CHECK_MSG(p->hasError(),
                       "gateway must reject transfer-codings it would otherwise drop");
    }
    {
        std::string raw =
            "POST /stream HTTP/1.1\r\n"
            "Host: x\r\n"
            "Transfer-Encoding: chunked, gzip\r\n"
            "\r\n";
        auto p = parse_all(raw);
        TEST_CHECK_MSG(p->hasError(),
                       "chunked must be the final transfer-coding");
    }
}

// 4. 半包到达 → 累积到完整头块(\r\n\r\n)后单次解析(模拟 Layer2 InBuf 的契约)
//    Mongrel 非增量:必须攒齐头块再解析。这里验证"攒齐后解析正确"。
static void test_accumulate_then_parse() {
    std::string raw =
        "GET /p HTTP/1.1\r\n"
        "Host: x\r\n"
        "\r\n";
    // 模拟分片到达:逐片 append 到累积缓冲,检测到 \r\n\r\n 才解析
    std::string acc;
    HttpReqParser::ptr p;
    const size_t step = 5;
    for(size_t i = 0; i < raw.size(); i += step) {
        acc.append(raw, i, std::min(step, raw.size() - i));
        if(acc.find("\r\n\r\n") != std::string::npos) {
            p = std::make_shared<HttpReqParser>();
            p->execute(acc.data(), acc.size());
            break;
        }
    }
    TEST_CHECK_MSG(p && p->isFinished(), "accumulate-then-parse should finish");
    TEST_CHECK_MSG(p && !p->hasError(), "no error");
    TEST_CHECK_MSG(p && p->getData()->getPath() == "/p", "path after accumulate");
}

// 5. 走私:CL + TE 同时出现 → 拒绝
static void test_smuggling_cl_te() {
    std::string raw =
        "POST /x HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    auto p = parse_all(raw);
    p->getBodyFraming();   // 触发走私判定
    TEST_CHECK_MSG(p->hasError(), "CL+TE should be rejected as smuggling");
}

// 6. 走私:重复且不一致的 Content-Length → 拒绝
static void test_smuggling_dup_cl() {
    std::string raw =
        "POST /x HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 6\r\n"
        "\r\n";
    auto p = parse_all(raw);
    TEST_CHECK_MSG(p->hasError(), "inconsistent dup CL should be rejected");
}

// 7. 不认识的方法:透传(不报错,raw 保留)
static void test_unknown_method() {
    std::string raw =
        "PROPFIND /dav HTTP/1.1\r\n"
        "Host: x\r\n"
        "\r\n";
    auto p = parse_all(raw);
    TEST_CHECK_MSG(p->isFinished(), "finish");
    TEST_CHECK_MSG(!p->hasError(), "unknown method should NOT error (transparent)");
    auto req = p->getData();
    TEST_CHECK_MSG(req->getMethod() == HttpMethod::INVALID_METHOD, "method=INVALID");
    TEST_CHECK_MSG(req->getMethodRaw() == "PROPFIND", "raw method preserved: " << req->getMethodRaw());
}

static void test_response_duplicate_headers_and_framing() {
    {
        std::string raw =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Set-Cookie: a=1\r\n"
            "Set-Cookie: b=2\r\n"
            "\r\n";
        auto p = parse_rsp(raw);
        TEST_CHECK_MSG(p->isFinished(), "response parser should finish");
        TEST_CHECK_MSG(!p->hasError(), "response parser should accept duplicate Set-Cookie");
        auto rsp = p->getData();
        int cookies = 0;
        for(const auto& h : rsp->getHeaderList()) {
            if(strcasecmp(h.first.c_str(), "Set-Cookie") == 0) {
                ++cookies;
            }
        }
        TEST_CHECK_EQ(cookies, 2);
    }
    {
        std::string raw =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Content-Length: 6\r\n"
            "\r\n";
        auto p = parse_rsp(raw);
        TEST_CHECK_MSG(p->hasError(), "inconsistent duplicate response CL rejected");
    }
    {
        std::string raw =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n";
        auto p = parse_rsp(raw);
        TEST_CHECK_MSG(p->hasError(), "response CL+TE rejected");
    }
}

static void test_response_vary_merge() {
    auto rsp = std::make_shared<GwResponse>();
    rsp->setHeader("Vary", "Accept-Encoding");
    rsp->setHeader("Vary", "Origin");
    rsp->setHeader("Vary", "origin");
    std::string vary = rsp->getHeader("Vary");
    TEST_CHECK_MSG(vary.find("Accept-Encoding") != std::string::npos,
                   "existing Vary token preserved: " << vary);
    TEST_CHECK_MSG(vary.find("Origin") != std::string::npos,
                   "Origin Vary token added: " << vary);
    TEST_CHECK_MSG(vary.find("Origin, origin") == std::string::npos,
                   "Vary token should not be duplicated case-insensitively: " << vary);
}

static void test_request_xff_merge() {
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: x\r\n"
        "X-Forwarded-For: 198.51.100.4\r\n"
        "X-Forwarded-For: 10.1.2.3\r\n"
        "\r\n";
    auto p = parse_all(raw);
    TEST_CHECK(!p->hasError());
    auto req = p->getData();
    TEST_CHECK_EQ(req->getHeader("X-Forwarded-For"), std::string("198.51.100.4, 10.1.2.3"));
    TEST_CHECK_EQ(req->getHeaderList().size(), 3u);
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_gw_parser start ===";
    test_basic_get();
    test_content_length();
    test_chunked();
    test_transfer_encoding_token_boundary();
    test_accumulate_then_parse();
    test_smuggling_cl_te();
    test_smuggling_dup_cl();
    test_unknown_method();
    test_response_duplicate_headers_and_framing();
    test_response_vary_merge();
    test_request_xff_merge();
    return TEST_SUMMARY();
}
