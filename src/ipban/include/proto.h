#pragma once

// UDS 上的定长帧头 + 变长 body。一次 send 不等于一条消息, 一次 recv 也不等于,
// 所以收发都得循环凑满。body 用 JSON(复用 jsoncpp), 跨进程绝不直接甩 C++ 对象。

#include "net_socket.h"
#include <cstdint>
#include <string>

namespace bronx {
namespace ipban {

constexpr uint32_t kMagic   = 0x42414e31;   // 'BAN1'
constexpr uint16_t kVersion = 2;
constexpr uint32_t kMaxBody = 8 * 1024 * 1024;   // body 上限, 防恶意超长分配

// 消息种类。骨干版够用: 打招呼 / 举报 / 要全量 / 全量 / 确认 / 心跳 / 出错。
enum class Kind : uint16_t {
    HELLO    = 1,   // gateway -> daemon, 报 instanceId + 已应用版本
    RISK     = 2,   // producer -> daemon, 一条举报
    SNAP_REQ = 3,   // gateway -> daemon, 要全量快照
    SNAP     = 4,   // daemon -> gateway, 全量规则 + 版本
    ACK      = 5,   // gateway -> daemon, 应用完某版本
    PING     = 6,
    ERR      = 7,
    DELTA    = 8,   // daemon -> gateway, 增量(prevVer 对得上才应用)
    PONG     = 9,
    READY    = 10,
    BYE      = 11,
    PUT      = 12,
    DEL      = 13,
    LIST     = 14,
    RESULT   = 15,
};

// 定长头, 网络字节序收发。sequence 给调试对账用。
struct Head {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    uint16_t kind = 0;
    uint32_t bodyLen = 0;
    uint64_t seq = 0;
};
constexpr size_t kHeadSize = 4 + 2 + 2 + 4 + 8;   // 20 字节, 别信 sizeof(结构体对齐)

// 收发结果, 调用方据此决定重连还是继续。
enum class MsgRet { OK, CLOSED, BADMSG, IOERR };

enum class IoErr {
    NONE, CLOSED, CANCELED, TIMEOUT, SYSTEM
};

enum class FrameErr {
    NONE, SHORT_HEAD, BAD_MAGIC, BAD_VER, BAD_KIND, TOO_BIG, SHORT_BODY
};

enum class MsgErr {
    NONE, BAD_JSON, BAD_FIELD, BAD_STATE, BAD_ACK, BAD_PONG, VER_GAP
};

struct ProtoErr {
    IoErr io = IoErr::NONE;
    FrameErr frame = FrameErr::NONE;
    int sys = 0;
};

// 一条完整消息: 头 + body。
struct Msg {
    Kind        kind = Kind::PING;
    uint64_t    seq = 0;
    std::string body;   // JSON 文本, 可空
};

// 写满一段字节, 处理 EINTR / 短写 / hook 的 EAGAIN。成功返回 true。
bool sendAll(const bronx::BxSocket::ptr& sock, const void* data, size_t len,
             ProtoErr* err = nullptr);
// 读满 len 字节。对端关闭 -> CLOSED, 出错 -> IOERR。
MsgRet recvAll(const bronx::BxSocket::ptr& sock, void* data, size_t len,
               ProtoErr* err = nullptr);

// 发一条消息(头+body 一把发)。
bool sendMsg(const bronx::BxSocket::ptr& sock, Kind kind, const std::string& body,
             uint64_t seq = 0, ProtoErr* err = nullptr);
// 收一条消息, 校验 magic/version/长度上限。
MsgRet recvMsg(const bronx::BxSocket::ptr& sock, Msg& out, ProtoErr* err = nullptr);

// 头的打包/解包, 手动按字节序摆, 不靠结构体内存布局。
void   packHead(const Head& h, uint8_t buf[kHeadSize]);
bool   unpackHead(const uint8_t buf[kHeadSize], Head& h);
FrameErr headErr(const uint8_t buf[kHeadSize], Head& h);

} // namespace ipban
} // namespace bronx
