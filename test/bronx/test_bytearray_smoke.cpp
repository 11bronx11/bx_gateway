// BxByteArray「日常使用」稳定性冒烟测试
// ===================================
// 目标：模拟 BxByteArray 在框架中的真实用法，验证常规路径下稳定可用。
// 场景贴近实际：
//   1. 协议打包/解包往返：把一条"消息"(定长头 + varint 长度 + 字符串 + 数值字段)
//      写入 BxByteArray，再原样读出，循环大量次数。
//   2. iovec 收发模拟：用 peekWriteIovec 拿写缓冲、memcpy 进数据、setPosition 提交，
//      再用 peekReadIovec 拿读缓冲取出，校验一致（echo_server 的典型用法）。
//   3. 缓冲复用：同一个 BxByteArray clear 后反复打包不同消息。
//   4. 多种 base_size（1/13/4096）下重复上述，覆盖跨 Node 边界。

#include "bytearray.h"
#include "log.h"
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <random>

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

// 一条业务消息
struct Msg {
    uint8_t  type;
    uint32_t id;          // varint
    int64_t  value;       // varint(zigzag)
    double   score;
    std::string name;     // 变长字符串
};

static void pack(bronx::BxByteArray& ba, const Msg& m){
    ba.writeFuint8(m.type);
    ba.writeUint32(m.id);
    ba.writeInt64(m.value);
    ba.writeDouble(m.score);
    ba.writeVarStr(m.name);
}

static Msg unpack(bronx::BxByteArray& ba){
    Msg m;
    m.type  = ba.readFuint8();
    m.id    = ba.readUint32();
    m.value = ba.readInt64();
    m.score = ba.readDouble();
    m.name  = ba.readVarStr();
    return m;
}

static bool eq(const Msg& a, const Msg& b){
    return a.type == b.type && a.id == b.id && a.value == b.value
        && a.score == b.score && a.name == b.name;
}


// ===== 场景一：大量消息打包/解包往返 =====
static void test_protocol_roundtrip(size_t base_size){
    std::mt19937_64 rng(12345 + base_size);
    const int N = 5000;
    std::vector<Msg> msgs;
    bronx::BxByteArray ba(base_size);

    for(int i = 0; i < N; ++i){
        Msg m;
        m.type  = (uint8_t)(rng() & 0xff);
        m.id    = (uint32_t)rng();
        m.value = (int64_t)rng();
        m.score = (double)((int64_t)rng()) / 1000.0;
        size_t len = rng() % 64;          // 0~63 字节名字（含跨 Node）
        m.name.assign(len, 'a' + (char)(i % 26));
        msgs.push_back(m);
        pack(ba, m);
    }

    ba.setPosition(0);
    for(int i = 0; i < N; ++i){
        Msg got = unpack(ba);
        if(!eq(got, msgs[i])){
            EXPECT(false);
            break;
        }
    }
    EXPECT(ba.getReadSize() == 0);     // 正好读完，无残留
}


// ===== 场景二：iovec 收发模拟（echo_server 典型用法）=====
static void test_iovec_io(size_t base_size){
    // 构造一段载荷
    std::string payload;
    payload.reserve(20000);
    for(int i = 0; i < 20000; ++i) payload.push_back((char)('A' + (i % 26)));

    // 写入端：用 peekWriteIovec 拿可写 iovec，memcpy 进去，再 setPosition 提交
    bronx::BxByteArray ba(base_size);
    std::vector<iovec> wb;
    ba.peekWriteIovec(wb, payload.size());
    size_t off = 0;
    for(auto& v : wb){
        size_t n = std::min(v.iov_len, payload.size() - off);
        std::memcpy(v.iov_base, payload.data() + off, n);
        off += n;
    }
    EXPECT(off == payload.size());
    // 提交写入的数据量（更新 m_size / m_position）
    ba.setPosition(ba.getPosition() + payload.size());
    EXPECT(ba.getSize() == payload.size());

    // 读取端：position 回 0，用 peekReadIovec 拿可读 iovec 取出
    ba.setPosition(0);
    std::vector<iovec> rb;
    uint64_t rn = ba.peekReadIovec(rb, payload.size());
    EXPECT(rn == payload.size());
    std::string out;
    out.reserve(payload.size());
    for(auto& v : rb){
        out.append((char*)v.iov_base, v.iov_len);
    }
    EXPECT(out == payload);
    // toString 也应一致
    EXPECT(ba.toString() == payload);
}


// ===== 场景三：缓冲复用（clear 后反复使用）=====
static void test_reuse(size_t base_size){
    bronx::BxByteArray ba(base_size);
    for(int round = 0; round < 500; ++round){
        ba.clear();
        EXPECT(ba.getSize() == 0);
        Msg m;
        m.type = (uint8_t)round;
        m.id = (uint32_t)(round * 7 + 1);
        m.value = -(int64_t)round * 13;
        m.score = round * 0.5;
        m.name = std::string(round % 40, 'z');
        pack(ba, m);
        ba.setPosition(0);
        Msg got = unpack(ba);
        if(!eq(got, m)){ EXPECT(false); break; }
        EXPECT(ba.getReadSize() == 0);
    }
}


int main(){
    BRONX_LOG_INFO(g_logger) << "==== BxByteArray daily-use smoke test start ====";

    for(size_t bs : {1u, 13u, 4096u}){
        BRONX_LOG_INFO(g_logger) << "--- base_size=" << bs << " ---";
        test_protocol_roundtrip(bs);
        test_iovec_io(bs);
        test_reuse(bs);
    }

    BRONX_LOG_INFO(g_logger) << "==== checks=" << g_checks
                             << " failed=" << g_failed << " ====";
    if(g_failed){
        BRONX_LOG_ERROR(g_logger) << "SMOKE TEST FAILED: " << g_failed;
        return 1;
    }
    BRONX_LOG_INFO(g_logger) << "ALL BYTEARRAY SMOKE TESTS PASS";
    return 0;
}
