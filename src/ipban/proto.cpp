#include "proto.h"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>

namespace bronx {
namespace ipban {

// 大端摆放, 不依赖机器字节序
static void put32(uint8_t*& p, uint32_t v) {
    *p++ = (v >> 24) & 0xff; *p++ = (v >> 16) & 0xff;
    *p++ = (v >> 8) & 0xff;  *p++ = v & 0xff;
}
static void put16(uint8_t*& p, uint16_t v) {
    *p++ = (v >> 8) & 0xff; *p++ = v & 0xff;
}
static void put64(uint8_t*& p, uint64_t v) {
    for(int i = 7; i >= 0; --i) *p++ = (v >> (i * 8)) & 0xff;
}
static uint32_t get32(const uint8_t*& p) {
    uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
               | ((uint32_t)p[2] << 8) | p[3];
    p += 4; return v;
}
static uint16_t get16(const uint8_t*& p) {
    uint16_t v = (uint16_t)((p[0] << 8) | p[1]); p += 2; return v;
}
static uint64_t get64(const uint8_t*& p) {
    uint64_t v = 0;
    for(int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    p += 8; return v;
}

static bool knownKind(uint16_t kind) {
    switch((Kind)kind) {
        case Kind::HELLO:
        case Kind::RISK:
        case Kind::SNAP_REQ:
        case Kind::SNAP:
        case Kind::ACK:
        case Kind::PING:
        case Kind::ERR:
        case Kind::DELTA:
        case Kind::PONG:
        case Kind::READY:
        case Kind::BYE:
        case Kind::PUT:
        case Kind::DEL:
        case Kind::LIST:
        case Kind::RESULT:
            return true;
    }
    return false;
}

void packHead(const Head& h, uint8_t buf[kHeadSize]) {
    uint8_t* p = buf;
    put32(p, h.magic);
    put16(p, h.version);
    put16(p, h.kind);
    put32(p, h.bodyLen);
    put64(p, h.seq);
}

FrameErr headErr(const uint8_t buf[kHeadSize], Head& h) {
    const uint8_t* p = buf;
    h.magic   = get32(p);
    h.version = get16(p);
    h.kind    = get16(p);
    h.bodyLen = get32(p);
    h.seq     = get64(p);
    if(h.magic != kMagic) return FrameErr::BAD_MAGIC;
    if(h.version != kVersion) return FrameErr::BAD_VER;
    if(!knownKind(h.kind)) return FrameErr::BAD_KIND;
    if(h.bodyLen > kMaxBody) return FrameErr::TOO_BIG;
    return FrameErr::NONE;
}

bool unpackHead(const uint8_t buf[kHeadSize], Head& h) {
    return headErr(buf, h) == FrameErr::NONE;
}

static IoErr ioErr(int e) {
    if(e == ECANCELED) return IoErr::CANCELED;
    if(e == ETIMEDOUT || e == EAGAIN || e == EWOULDBLOCK) return IoErr::TIMEOUT;
    if(e == EPIPE || e == ECONNRESET || e == ENOTCONN) return IoErr::CLOSED;
    return IoErr::SYSTEM;
}

static void setIo(ProtoErr* err, IoErr io, int sys = 0) {
    if(!err) return;
    err->io = io;
    err->sys = sys;
}

static MsgRet readExact(const bronx::BxSocket::ptr& sock, void* data, size_t len,
                        size_t& got, ProtoErr* err) {
    got = 0;
    if(err) *err = {};
    if(!sock || (!data && len > 0)) {
        errno = EINVAL;
        setIo(err, IoErr::SYSTEM, errno);
        return MsgRet::IOERR;
    }
    char* p = static_cast<char*>(data);
    while(got < len) {
        int n = sock->recv(p + got, len - got);
        if(n < 0 && errno == EINTR) continue;
        if(n == 0) {
            setIo(err, IoErr::CLOSED);
            return MsgRet::CLOSED;
        }
        if(n < 0) {
            setIo(err, ioErr(errno), errno);
            return MsgRet::IOERR;
        }
        got += static_cast<size_t>(n);
    }
    return MsgRet::OK;
}

bool sendAll(const bronx::BxSocket::ptr& sock, const void* data, size_t len,
             ProtoErr* err) {
    if(err) *err = {};
    if(!sock || (!data && len > 0)) {
        errno = EINVAL;
        setIo(err, IoErr::SYSTEM, errno);
        return false;
    }
    const char* p = (const char*)data;
    size_t sent = 0;
    while(sent < len) {
        int n = sock->send(p + sent, len - sent, MSG_NOSIGNAL);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0) {
            setIo(err, ioErr(errno), errno);
            return false;
        }
        if(n == 0) {
            setIo(err, IoErr::CLOSED);
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

MsgRet recvAll(const bronx::BxSocket::ptr& sock, void* data, size_t len,
               ProtoErr* err) {
    size_t got = 0;
    return readExact(sock, data, len, got, err);
}

bool sendMsg(const bronx::BxSocket::ptr& sock, Kind kind,
             const std::string& body, uint64_t seq, ProtoErr* err) {
    if(!knownKind((uint16_t)kind)) {
        errno = EPROTO;
        if(err) {
            *err = {};
            err->frame = FrameErr::BAD_KIND;
            err->sys = errno;
        }
        return false;
    }
    if(body.size() > kMaxBody) {
        errno = EMSGSIZE;
        if(err) {
            *err = {};
            err->frame = FrameErr::TOO_BIG;
            err->sys = errno;
        }
        return false;
    }
    Head h;
    h.kind = (uint16_t)kind;
    h.bodyLen = (uint32_t)body.size();
    h.seq = seq;
    uint8_t hb[kHeadSize];
    packHead(h, hb);
    // 头 + body 拼一把发, 省一次 syscall 也避免半头半 body
    std::string out;
    out.reserve(kHeadSize + body.size());
    out.append((const char*)hb, kHeadSize);
    out.append(body);
    return sendAll(sock, out.data(), out.size(), err);
}

MsgRet recvMsg(const bronx::BxSocket::ptr& sock, Msg& out, ProtoErr* err) {
    if(err) *err = {};
    uint8_t hb[kHeadSize];
    size_t got = 0;
    MsgRet r = readExact(sock, hb, kHeadSize, got, err);
    if(r != MsgRet::OK) {
        if(got > 0) {
            if(err) err->frame = FrameErr::SHORT_HEAD;
            return MsgRet::BADMSG;
        }
        return r;
    }
    Head h;
    FrameErr fe = headErr(hb, h);
    if(fe != FrameErr::NONE) {
        if(err) err->frame = fe;
        return MsgRet::BADMSG;
    }
    out.kind = (Kind)h.kind;
    out.seq = h.seq;
    out.body.clear();
    if(h.bodyLen > 0) {
        out.body.resize(h.bodyLen);
        got = 0;
        r = readExact(sock, out.body.data(), h.bodyLen, got, err);
        if(r != MsgRet::OK) {
            if(got > 0 || r == MsgRet::CLOSED) {
                if(err) err->frame = FrameErr::SHORT_BODY;
                return MsgRet::BADMSG;
            }
            return r;
        }
    }
    return MsgRet::OK;
}

} // namespace ipban
} // namespace bronx
