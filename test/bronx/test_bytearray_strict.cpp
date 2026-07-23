// BxByteArray 严苛/边界测试
// 覆盖：定长整数、varint(zigzag)、float/double、字符串、读写指针、
//       多 Node 跨界、clear、文件往返、iovec、越界异常
// 重点确定性边界：127/16383 等会让 varint 末组==0x7f 的值；
//                  非 base_size=1 下的 writeToFile/readFromFile。

#include "bytearray.h"
#include "log.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <limits>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_ROOT();
static int g_failed = 0;
static int g_checks = 0;

#define EXPECT(expr)                                                          \
    do {                                                                      \
        ++g_checks;                                                           \
        if(!(expr)) {                                                         \
            BRONX_LOG_ERROR(g_logger) << "FAIL: " #expr                       \
                                      << " @ " << __FILE__ << ":" << __LINE__;\
            ++g_failed;                                                       \
        }                                                                     \
    } while(0)

// ===== varint 边界：末 7-bit 组恰好为 0x7f 的值 =====
// 0x7f=127(单字节 0x7f)、16383=0x3FFF(双字节 0xFF 0x7F)、
// 2097151=0x1FFFFF(三字节末组 0x7F) 等，都会让有 bug 的解码越界。
static void test_varint_boundary(){
    BRONX_LOG_INFO(g_logger) << "--- test_varint_boundary ---";
    uint32_t vals32[] = {0u, 1u, 126u, 127u, 128u, 255u, 16383u, 16384u,
                         2097151u, 2097152u, 0x7fffffffu, 0xffffffffu};
    for(uint32_t v : vals32){
        bronx::BxByteArray ba(1);
        ba.writeUint32(v);
        ba.setPosition(0);
        uint32_t r = ba.readUint32();
        EXPECT(r == v);
        if(r != v) BRONX_LOG_ERROR(g_logger) << "u32 v=" << v << " got=" << r;
    }
    int32_t svals32[] = {0, -1, 1, 63, -64, 127, -128, 8191, -8192,
                         std::numeric_limits<int32_t>::max(),
                         std::numeric_limits<int32_t>::min()};
    for(int32_t v : svals32){
        bronx::BxByteArray ba(1);
        ba.writeInt32(v);
        ba.setPosition(0);
        EXPECT(ba.readInt32() == v);
    }
    uint64_t vals64[] = {0ull, 127ull, 128ull, 16383ull, 2097151ull,
                         (1ull<<35)-1, std::numeric_limits<uint64_t>::max()};
    for(uint64_t v : vals64){
        bronx::BxByteArray ba(1);
        ba.writeUint64(v);
        ba.setPosition(0);
        EXPECT(ba.readUint64() == v);
    }
    int64_t svals64[] = {0, -1, 127, -128, std::numeric_limits<int64_t>::max(),
                         std::numeric_limits<int64_t>::min()};
    for(int64_t v : svals64){
        bronx::BxByteArray ba(1);
        ba.writeInt64(v);
        ba.setPosition(0);
        EXPECT(ba.readInt64() == v);
    }
}

// ===== 定长整数 + 字节序 =====
static void test_fixed_ints(){
    BRONX_LOG_INFO(g_logger) << "--- test_fixed_ints ---";
    for(int le = 0; le <= 1; ++le){
        bronx::BxByteArray ba(1);
        ba.setIsLittleEndian(le);
        ba.writeFint8(-128);
        ba.writeFuint8(255);
        ba.writeFint16(std::numeric_limits<int16_t>::min());
        ba.writeFuint16(0xABCD);
        ba.writeFint32(std::numeric_limits<int32_t>::min());
        ba.writeFuint32(0xDEADBEEF);
        ba.writeFint64(std::numeric_limits<int64_t>::min());
        ba.writeFuint64(0x1122334455667788ull);
        ba.setPosition(0);
        EXPECT(ba.readFint8() == -128);
        EXPECT(ba.readFuint8() == 255);
        EXPECT(ba.readFint16() == std::numeric_limits<int16_t>::min());
        EXPECT(ba.readFuint16() == 0xABCD);
        EXPECT(ba.readFint32() == std::numeric_limits<int32_t>::min());
        EXPECT(ba.readFuint32() == 0xDEADBEEF);
        EXPECT(ba.readFint64() == std::numeric_limits<int64_t>::min());
        EXPECT(ba.readFuint64() == 0x1122334455667788ull);
        EXPECT(ba.getReadSize() == 0);
    }
}

// ===== float / double =====
static void test_float_double(){
    BRONX_LOG_INFO(g_logger) << "--- test_float_double ---";
    float fs[] = {0.0f, -0.0f, 1.5f, -3.1415926f, 1e30f, -1e-30f};
    double ds[] = {0.0, -2.718281828, 1e300, -1e-300};
    bronx::BxByteArray ba(3);
    for(float f : fs) ba.writeFloat(f);
    for(double d : ds) ba.writeDouble(d);
    ba.setPosition(0);
    for(float f : fs) EXPECT(ba.readFloat() == f);
    for(double d : ds) EXPECT(ba.readDouble() == d);
}

// ===== 字符串：空/含\0/跨 Node =====
static void test_strings(){
    BRONX_LOG_INFO(g_logger) << "--- test_strings ---";
    std::string empty = "";
    std::string nul = std::string("a\0b\0c", 5);   // 含内嵌 \0
    std::string big(10000, 'x');                    // 跨多个 Node
    for(size_t bs : {1u, 7u, 4096u}){
        bronx::BxByteArray ba(bs);
        ba.writeStringF16(empty);
        ba.writeStringF32(nul);
        ba.writeStringF64(big);
        ba.writeVarStr(nul);
        ba.writeRawStr(big);
        ba.setPosition(0);
        EXPECT(ba.readStringF16() == empty);
        EXPECT(ba.readStringF32() == nul);
        EXPECT(ba.readStringF64() == big);
        EXPECT(ba.readVarStr() == nul);
        std::string tail; tail.resize(big.size());
        ba.read(&tail[0], tail.size());
        EXPECT(tail == big);
    }
}

// ===== position / 不动指针读 / toString =====
static void test_position_read(){
    BRONX_LOG_INFO(g_logger) << "--- test_position_read ---";
    bronx::BxByteArray ba(4);
    for(int i = 0; i < 20; ++i) ba.writeFuint8((uint8_t)i);
    EXPECT(ba.getSize() == 20);
    // 不动指针 read(buf,size,pos)
    uint8_t b = 0;
    ba.read(&b, 1, 10);
    EXPECT(b == 10);
    EXPECT(ba.getPosition() == 20);   // 不动指针不改变 position
    // toString 取 [position, size)
    ba.setPosition(5);
    EXPECT(ba.getReadSize() == 15);
    std::string s = ba.toString();
    EXPECT(s.size() == 15 && (uint8_t)s[0] == 5);
}

// ===== clear 后复用 =====
static void test_clear(){
    BRONX_LOG_INFO(g_logger) << "--- test_clear ---";
    bronx::BxByteArray ba(4);
    for(int i = 0; i < 50; ++i) ba.writeFuint32(i);
    ba.clear();
    EXPECT(ba.getSize() == 0);
    EXPECT(ba.getPosition() == 0);
    ba.writeFuint32(42);
    ba.setPosition(0);
    EXPECT(ba.readFuint32() == 42);
}

// ===== 越界异常 =====
static void test_out_of_range(){
    BRONX_LOG_INFO(g_logger) << "--- test_out_of_range ---";
    bronx::BxByteArray ba(8);
    ba.writeFuint8(1);
    ba.setPosition(0);
    bool threw = false;
    try { uint32_t v; ba.read(&v, 4); }  // 只有 1 字节可读
    catch(const std::out_of_range&) { threw = true; }
    EXPECT(threw);
    // setPosition 越界
    threw = false;
    try { ba.setPosition(ba.getSize() + 100000); }
    catch(const std::out_of_range&) { threw = true; }
    EXPECT(threw);
}

// ===== 文件往返：非 base_size=1（暴露 writeToFile 偏移 bug）=====
static void test_file_roundtrip(){
    BRONX_LOG_INFO(g_logger) << "--- test_file_roundtrip ---";
    for(size_t bs : {1u, 7u, 64u, 4096u}){
        bronx::BxByteArray ba(bs);
        std::string data;
        for(int i = 0; i < 1000; ++i){ ba.writeFuint32(i); }
        ba.setPosition(0);
        std::string expect = ba.toString();
        char path[128];
        std::snprintf(path, sizeof(path), "/tmp/ba_strict_%zu.dat", bs);
        ba.setPosition(0);
        EXPECT(ba.writeToFile(path));
        bronx::BxByteArray ba2(bs);
        EXPECT(ba2.readFromFile(path));
        ba2.setPosition(0);
        EXPECT(ba2.toString() == expect);
        std::remove(path);
    }

    // 最毒的用例:小数据 + 偏移落在节点中间,剩余仅几字节(旧公式 len=min(bs,read_size)-diff
    // 在此变负 → ofs.write 收到巨大 size_t 崩溃)。bs=7 写 12 字节,从 pos=10 写出剩 2 字节。
    {
        bronx::BxByteArray ba(7);
        for(int i = 0; i < 3; ++i){ ba.writeFuint32(i); }  // 12 字节,跨 2 个 7 字节节点
        ba.setPosition(10);
        std::string expect = ba.toString();   // 剩 2 字节
        EXPECT(expect.size() == 2);
        const char* path = "/tmp/ba_tiny_off.dat";
        ba.setPosition(10);
        EXPECT(ba.writeToFile(path));
        bronx::BxByteArray ba2(7);
        EXPECT(ba2.readFromFile(path));
        ba2.setPosition(0);
        EXPECT(ba2.toString() == expect);
        std::remove(path);
    }

    // 关键补充:position_ != 0 且未对齐节点边界的往返(覆盖原 writeToFile 被 setPosition(0) 掩盖的偏移 bug)。
    // baseSize 非 1 时,从任意中间偏移写出,期望 == 从该偏移起的 toString。
    for(size_t bs : {7u, 64u, 4096u}){
        for(size_t off : {1u, 3u, 5u, 100u, 333u}){
            bronx::BxByteArray ba(bs);
            for(int i = 0; i < 1000; ++i){ ba.writeFuint32(i); }
            ba.setPosition(off);
            std::string expect = ba.toString();   // [off, size)
            char path[128];
            std::snprintf(path, sizeof(path), "/tmp/ba_off_%zu_%zu.dat", bs, off);
            ba.setPosition(off);
            EXPECT(ba.writeToFile(path));
            bronx::BxByteArray ba2(bs);
            EXPECT(ba2.readFromFile(path));
            ba2.setPosition(0);
            EXPECT(ba2.toString() == expect);
            std::remove(path);
        }
    }
}

// ===== iovec：读/写缓冲 =====
static void test_iovec(){
    BRONX_LOG_INFO(g_logger) << "--- test_iovec ---";
    bronx::BxByteArray ba(4);
    std::string msg;
    for(int i = 0; i < 100; ++i){
        msg.push_back((char)('A' + (i % 26)));
    }
    ba.write(msg.data(), msg.size());
    ba.setPosition(0);
    std::vector<iovec> rb;
    uint64_t n = ba.peekReadIovec(rb, ~0ull);
    EXPECT(n == 100);
    uint64_t total = 0;
    for(auto& v : rb) total += v.iov_len;
    EXPECT(total == 100);
    // 从 position 偏移读：显式 position 不能受当前 m_position 影响
    ba.setPosition(95);
    std::vector<iovec> rb2;
    uint64_t n2 = ba.peekReadIovec(rb2, 50, 30);
    EXPECT(n2 == 50);
    std::string out;
    for(auto& v : rb2){
        out.append((char*)v.iov_base, v.iov_len);
    }
    EXPECT(out == msg.substr(30, 50));

    std::vector<iovec> rb3;
    EXPECT(ba.peekReadIovec(rb3, 10, msg.size()) == 0);
    std::vector<iovec> rb4;
    EXPECT(ba.peekReadIovec(rb4, 10, msg.size() + 1) == 0);
}

// PLACEHOLDER
int main(){
    BRONX_LOG_INFO(g_logger) << "==== BxByteArray test start ====";
    test_varint_boundary();
    test_fixed_ints();
    test_float_double();
    test_strings();
    test_position_read();
    test_clear();
    test_out_of_range();
    test_file_roundtrip();
    test_iovec();
    BRONX_LOG_INFO(g_logger) << "==== checks=" << g_checks
                             << " failed=" << g_failed << " ====";
    if(g_failed){ BRONX_LOG_ERROR(g_logger) << "BYTEARRAY TESTS FAILED: " << g_failed; return 1; }
    BRONX_LOG_INFO(g_logger) << "ALL BYTEARRAY TESTS PASS";
    return 0;
}
