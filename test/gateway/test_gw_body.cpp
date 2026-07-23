// G1 回归:InBuf + BodyReader/Encoder
// =============================================
// InBuf:append/consume/扩容/findHeaderEnd 边界。
// BodyReader:CL 定长读、chunked 解码(用 socketpair 喂数据)。
// BodyEncoder:chunked 编码格式。

#include "test_util.h"
#include "buf.h"
#include "body.h"
#include "mempool.h"
#include "reactor.h"
#include "net_socket.h"
#include "fd_context.h"
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <atomic>
#include <new>

using namespace bronx::gateway;
static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();

// ---- InBuf ----
static void test_inputbuffer_basic() {
    InBuf b(8);
    b.append("hello", 5);
    TEST_CHECK_EQ((int)b.readable(), 5);
    TEST_CHECK_MSG(std::string(b.peek(), 5) == "hello", "peek");
    b.consume(2);
    TEST_CHECK_EQ((int)b.readable(), 3);
    TEST_CHECK_MSG(std::string(b.peek(), 3) == "llo", "after consume");
    // 触发扩容(超 8 字节)
    b.append("1234567890", 10);
    TEST_CHECK_EQ((int)b.readable(), 13);
    TEST_CHECK_MSG(std::string(b.peek(), 13) == "llo1234567890", "after grow");
}

static void test_inputbuffer_header_end() {
    InBuf b;
    b.append("GET / HTTP/1.1\r\nHost: x\r\n", 25);
    TEST_CHECK_EQ((int)b.findHeaderEnd(), 0);   // 还没 \r\n\r\n
    b.append("\r\n", 2);
    size_t he = b.findHeaderEnd();
    TEST_CHECK_MSG(he == 27, "header end at 27, got " << he);
}

static void test_inputbuffer_reclaim() {
    // 反复 append+consume 不应无限增长(左移回收)
    InBuf b(16);
    for(int i = 0; i < 100; ++i) {
        b.append("0123456789", 10);
        b.consume(10);
    }
    TEST_CHECK_EQ((int)b.readable(), 0);
    TEST_CHECK_MSG(b.capacity() <= 64, "capacity should stay small via reclaim, got " << b.capacity());
}

static void test_inputbuffer_zero_capacity() {
    InBuf b(0);
    b.append("x", 1);
    TEST_CHECK_EQ((int)b.readable(), 1);
    TEST_CHECK_MSG(std::string(b.peek(), 1) == "x", "zero-capacity buffer append");
}

static void test_inputbuffer_trim_empty_large() {
    InBuf b(4096);
    std::string big(100000, 'x');
    b.append(big.data(), big.size());
    b.consumeAll();
    size_t before = b.capacity();
    bool trimmed = b.trim(4096, 4096);
    TEST_CHECK_MSG(trimmed, "empty large buffer should trim, before=" << before
                   << " after=" << b.capacity());
    TEST_CHECK_EQ((int)b.readable(), 0);
    TEST_CHECK_MSG(b.capacity() >= 4096, "trim keeps min capacity");
    TEST_CHECK_MSG(b.capacity() <= 8192, "trim target should be small, got " << b.capacity());
}

static void test_inputbuffer_trim_preserves_unconsumed_data() {
    InBuf b(4096);
    std::string big(100000, 'x');
    std::string keep = "abcdefghijklmnopqrstuvwxyz";
    std::string prefix(5000, 'p');
    b.append(big.data(), big.size());
    b.consumeAll();
    b.append(prefix.data(), prefix.size());
    b.append(keep.data(), keep.size());
    b.consume(prefix.size());
    TEST_CHECK_MSG(std::string(b.peek(), b.readable()) == keep, "pre-trim readable data");
    size_t beforeReadable = b.readable();
    bool trimmed = b.trim(4096, 4096);
    TEST_CHECK(trimmed);
    TEST_CHECK_EQ((int)b.readable(), (int)beforeReadable);
    TEST_CHECK_MSG(std::string(b.peek(), b.readable()) == keep, "trim preserves data order");
    b.append("1234", 4);
    TEST_CHECK_MSG(std::string(b.peek(), b.readable()) == keep + "1234",
                   "append after trim");
    b.consume(keep.size());
    TEST_CHECK_MSG(std::string(b.peek(), b.readable()) == "1234",
                   "consume after trim");
}

static void test_inputbuffer_trim_hysteresis_noop() {
    InBuf b(16384);
    b.append("abc", 3);
    size_t before = b.capacity();
    bool trimmed = b.trim(8192, 4096);
    TEST_CHECK(!trimmed);
    TEST_CHECK_EQ((int)b.capacity(), (int)before);
    TEST_CHECK_MSG(std::string(b.peek(), b.readable()) == "abc", "noop keeps data");
}

static void test_inputbuffer_trim_large_readable_sets_target() {
    InBuf b(4096);
    std::string filler(200000, 'x');
    std::string data(70000, 'd');
    b.append(filler.data(), filler.size());
    b.consumeAll();
    b.append(data.data(), data.size());
    size_t beforeReadable = b.readable();
    bool trimmed = b.trim(8192, 4096);
    TEST_CHECK(trimmed);
    TEST_CHECK_EQ((int)b.readable(), (int)beforeReadable);
    TEST_CHECK_MSG(std::string(b.peek(), 16) == std::string(16, 'd'), "large data prefix");
    TEST_CHECK_MSG(b.capacity() >= beforeReadable + 4096,
                   "capacity must hold readable + reserve");
}

static void test_inputbuffer_trim_zero_args() {
    InBuf b(4096);
    std::string big(70000, 'z');
    b.append(big.data(), big.size());
    b.consumeAll();
    bool trimmed = b.trim(0, 0);
    TEST_CHECK(trimmed);
    TEST_CHECK_MSG(b.capacity() >= 1, "trim never leaves zero capacity");
    b.append("q", 1);
    TEST_CHECK_EQ((int)b.readable(), 1);
    TEST_CHECK_MSG(std::string(b.peek(), 1) == "q", "append after zero-arg trim");
}

static void test_inputbuffer_trim_alloc_failure_keeps_state() {
    InBuf b(4096);
    std::string big(200000, 'x');
    std::string prefix(5000, 'p');
    std::string keep = "trim-allocation-failure-keeps-readable-data";
    b.append(big.data(), big.size());
    b.consumeAll();
    b.append(prefix.data(), prefix.size());
    b.append(keep.data(), keep.size());
    b.consume(prefix.size());

    size_t beforeCap = b.capacity();
    size_t beforeReadable = b.readable();
    bool freed = false;
    bool threw = false;
    bool trimmed = true;
    MemAlloc::setAllocator(
        [](size_t) -> void* {
            throw std::bad_alloc();
        },
        [&freed](void* p, size_t) {
            freed = true;
            ::operator delete(p);
        });
    try {
        trimmed = b.trim(70000, 4096);
    } catch(...) {
        threw = true;
    }
    MemAlloc::setAllocator(nullptr, nullptr);

    TEST_CHECK(!threw);
    TEST_CHECK(!trimmed);
    TEST_CHECK(!freed);
    TEST_CHECK_EQ((int)b.capacity(), (int)beforeCap);
    TEST_CHECK_EQ((int)b.readable(), (int)beforeReadable);
    TEST_CHECK_MSG(std::string(b.peek(), b.readable()) == keep,
                   "allocation failure keeps old readable bytes");
}

// ---- BodyReader：body 已在 buffer 的场景(最常见:小 body 随头到达)----
// (跨 socket 的流式读在 G2/G3 端到端测试覆盖;此处聚焦解码正确性)
static void test_body_cl_in_buffer() {
    InBuf b;
    b.append("hello world", 11);
    BodyReader r(BodyFraming::CONTENT_LENGTH, 11, &b, nullptr);
    std::string out;
    int total = 0, n;
    while((n = r.readChunk(out)) > 0) total += n;
    TEST_CHECK_MSG(!r.hasError(), "CL no error");
    TEST_CHECK_MSG(r.isFinished(), "CL finished");
    TEST_CHECK_EQ(total, 11);
    TEST_CHECK_MSG(out == "hello world", "CL body got: " << out);
}

static void test_body_cl_truncated_in_buffer() {
    InBuf b;
    b.append("abc", 3);
    BodyReader r(BodyFraming::CONTENT_LENGTH, 5, &b, nullptr);
    std::string out;
    TEST_CHECK_EQ(r.readChunk(out), 3);
    TEST_CHECK_MSG(r.readChunk(out) < 0, "truncated CL should be an error");
    TEST_CHECK_MSG(r.hasError(), "truncated CL marked error");
}

static void test_body_chunked_in_buffer() {
    InBuf b;
    // 两个 chunk: "Wiki"(4) + "pedia"(5) + 末块
    std::string wire = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    b.append(wire.data(), wire.size());
    BodyReader r(BodyFraming::CHUNKED, 0, &b, nullptr);
    std::string out;
    int n;
    while((n = r.readChunk(out)) > 0) {}
    TEST_CHECK_MSG(!r.hasError(), "chunked no error");
    TEST_CHECK_MSG(r.isFinished(), "chunked finished");
    TEST_CHECK_MSG(out == "Wikipedia", "chunked decoded got: " << out);
}

static void test_body_chunked_invalid_edges() {
    {
        InBuf b;
        std::string wire = "4xyz\r\nWiki\r\n0\r\n\r\n";
        b.append(wire.data(), wire.size());
        BodyReader r(BodyFraming::CHUNKED, 0, &b, nullptr);
        std::string out;
        TEST_CHECK_MSG(r.readChunk(out) < 0, "chunk size with junk suffix rejected");
        TEST_CHECK_MSG(r.hasError(), "junk suffix marked error");
    }
    {
        InBuf b;
        std::string wire = "4\r\nWikiXX0\r\n\r\n";
        b.append(wire.data(), wire.size());
        BodyReader r(BodyFraming::CHUNKED, 0, &b, nullptr);
        std::string out;
        TEST_CHECK_MSG(r.readChunk(out) < 0, "chunk data must end with CRLF");
        TEST_CHECK_MSG(r.hasError(), "bad chunk CRLF marked error");
    }
}

static void test_body_chunked_limits_and_streaming() {
    {
        InBuf b;
        std::string wire = "a\r\n0123456789\r\n0\r\n\r\n";
        b.append(wire.data(), wire.size());
        BodyReader r(BodyFraming::CHUNKED, 0, &b, nullptr);
        r.setMaxBodySize(4);
        std::string out;
        TEST_CHECK_MSG(r.readChunk(out) < 0, "oversized chunk must fail before buffering body");
        TEST_CHECK_MSG(r.hasError(), "oversized chunk marked error");
        TEST_CHECK_MSG(r.errorCode() == BodyReadError::TOO_LARGE,
                       "oversized chunk error code should be TOO_LARGE");
        TEST_CHECK_MSG(out.empty(), "oversized chunk should not emit partial data");
    }
    {
        InBuf b;
        std::string wire = "8\r\nabcdefgh\r\n0\r\n\r\n";
        b.append(wire.data(), wire.size());
        BodyReader r(BodyFraming::CHUNKED, 0, &b, nullptr);
        std::string out;
        TEST_CHECK_EQ(r.readChunk(out), 8);
        TEST_CHECK_EQ(r.readChunk(out), 0);
        TEST_CHECK_MSG(out == "abcdefgh", "complete chunked body got: " << out);
    }
}

static void test_body_none() {
    InBuf b;
    BodyReader r(BodyFraming::NONE, 0, &b, nullptr);
    std::string out;
    TEST_CHECK_EQ(r.readChunk(out), 0);
    TEST_CHECK_MSG(r.isFinished(), "NONE finished immediately");
}

// ---- BodyEncoder ----
static void test_encoder_chunked() {
    std::string c = BodyEncoder::encodeChunk(BodyFraming::CHUNKED, "Wiki", 4);
    TEST_CHECK_MSG(c == "4\r\nWiki\r\n", "chunk encode got: [" << c << "]");
    std::string e = BodyEncoder::encodeEnd(BodyFraming::CHUNKED);
    TEST_CHECK_MSG(e == "0\r\n\r\n", "chunk end");
    // CL/identity 原样
    std::string id = BodyEncoder::encodeChunk(BodyFraming::CONTENT_LENGTH, "abc", 3);
    TEST_CHECK_MSG(id == "abc", "identity passthrough");
}

int main() {
    BRONX_LOG_INFO(g_logger) << "=== test_gw_body start ===";
    test_inputbuffer_basic();
    test_inputbuffer_header_end();
    test_inputbuffer_reclaim();
    test_inputbuffer_zero_capacity();
    test_inputbuffer_trim_empty_large();
    test_inputbuffer_trim_preserves_unconsumed_data();
    test_inputbuffer_trim_hysteresis_noop();
    test_inputbuffer_trim_large_readable_sets_target();
    test_inputbuffer_trim_zero_args();
    test_inputbuffer_trim_alloc_failure_keeps_state();
    test_body_cl_in_buffer();
    test_body_cl_truncated_in_buffer();
    test_body_chunked_in_buffer();
    test_body_chunked_invalid_edges();
    test_body_chunked_limits_and_streaming();
    test_body_none();
    test_encoder_chunked();
    return TEST_SUMMARY();
}
