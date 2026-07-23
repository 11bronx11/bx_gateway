// 只测握手 对账 超时 关停 dirty 推送
#include "test_util.h"
#include "daemon.h"
#include "pull.h"
#include "proto.h"
#include "wire.h"
#include "reactor.h"
#include "net_socket.h"
#include "endpoint.h"
#include "util.h"
#include "metrics.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace bronx::ipban;

static std::atomic<uint64_t> g_seq{10};

static bronx::BxSocket::ptr dial(const std::string& path, const std::string& inst,
                                 uint64_t epoch = 0, uint64_t ver = 0) {
    auto sock = bronx::BxSocket::MakeUnixTcpSocket();
    auto addr = bronx::BxUnixAddress::Create(path);
    if(!sock || !addr || !sock->connect(addr, 2000)) return nullptr;
    uint64_t seq = g_seq.fetch_add(1);
    if(!sendMsg(sock, Kind::HELLO, helloToJson(inst, epoch, ver), seq)) return nullptr;
    sock->setRecvTimeout(1500);
    return sock;
}

static bool readReady(const bronx::BxSocket::ptr& sock, uint64_t& epoch,
                      uint64_t& ver, Msg* out = nullptr) {
    Msg msg;
    if(recvMsg(sock, msg) != MsgRet::OK || msg.kind != Kind::READY || !msg.seq) return false;
    if(!readyFromJson(msg.body, epoch, ver)) return false;
    if(out) *out = std::move(msg);
    return true;
}

static bool readSnap(const bronx::BxSocket::ptr& sock, Msg& msg,
                     uint64_t& epoch, uint64_t& ver) {
    std::vector<Rule> rules;
    return recvMsg(sock, msg) == MsgRet::OK && msg.kind == Kind::SNAP && msg.seq
        && snapFromJson(msg.body, epoch, ver, rules);
}

static bool ack(const bronx::BxSocket::ptr& sock, const Msg& msg,
                const std::string& inst, uint64_t epoch, uint64_t ver) {
    return sendMsg(sock, Kind::ACK, helloToJson(inst, epoch, ver), msg.seq);
}

static bool readErrClose(const bronx::BxSocket::ptr& sock, const char* want) {
    Msg msg;
    std::string code;
    if(recvMsg(sock, msg) != MsgRet::OK || msg.kind != Kind::ERR
       || !errFromJson(msg.body, code) || code != want) return false;
    Msg closed;
    return recvMsg(sock, closed) == MsgRet::CLOSED;
}

// 往 submit 口发一条 risk, 快速返回
static bool submitRisk(const std::string& path, const char* id, const char* ipStr) {
    auto sock = bronx::BxSocket::MakeUnixTcpSocket();
    auto addr = bronx::BxUnixAddress::Create(path);
    if(!sock || !addr || !sock->connect(addr, 2000)) return false;
    Risk r; r.id = id; parseCidr(ipStr, r.ip); r.src = Src::RATE; r.banMs = 0;
    r.atMs = bronx::GetCurrentMs();
    bool ok = sendMsg(sock, Kind::RISK, riskToJson(r));
    sock->close();
    return ok;
}

// 收任意 SNAP 或 DELTA, 返回新版本
static bool readPush(const bronx::BxSocket::ptr& sock, Msg& msg, uint64_t& newVer) {
    if(recvMsg(sock, msg) != MsgRet::OK) return false;
    uint64_t ep = 0;
    if(msg.kind == Kind::SNAP) {
        std::vector<Rule> rules;
        return snapFromJson(msg.body, ep, newVer, rules);
    }
    if(msg.kind == Kind::DELTA) {
        uint64_t prev = 0;
        std::vector<DeltaOp> ops;
        return deltaFromJson(msg.body, ep, prev, newVer, ops);
    }
    return false;
}

static bool badFrame(const std::string& path, int which) {
    auto sock = bronx::BxSocket::MakeUnixTcpSocket();
    auto addr = bronx::BxUnixAddress::Create(path);
    if(!sock || !addr || !sock->connect(addr, 2000)) return false;
    Head head;
    head.kind = (uint16_t)Kind::HELLO;
    head.seq = 1;
    if(which == 1) head.bodyLen = 8;
    if(which == 2) head.magic ^= 1;
    if(which == 3) head.bodyLen = kMaxBody + 1;
    uint8_t raw[kHeadSize];
    packHead(head, raw);
    bool ok = true;
    if(which == 0) ok = sendAll(sock, raw, 7);
    else if(which == 1) ok = sendAll(sock, raw, kHeadSize) && sendAll(sock, "xx", 2);
    else ok = sendAll(sock, raw, kHeadSize);
    sock->close();
    return ok;
}

static bool fdRead(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while(len) {
        int n = ::recv(fd, p, len, 0);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool fdSend(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while(len) {
        int n = ::send(fd, p, len, MSG_NOSIGNAL);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool fdRecvMsg(int fd, Msg& msg) {
    uint8_t raw[kHeadSize];
    Head head;
    if(!fdRead(fd, raw, sizeof(raw)) || headErr(raw, head) != FrameErr::NONE) return false;
    msg.kind = static_cast<Kind>(head.kind);
    msg.seq = head.seq;
    msg.body.resize(head.bodyLen);
    return !head.bodyLen || fdRead(fd, msg.body.data(), msg.body.size());
}

static bool fdSendMsg(int fd, Kind kind, const std::string& body, uint64_t seq) {
    Head head;
    head.kind = static_cast<uint16_t>(kind);
    head.bodyLen = static_cast<uint32_t>(body.size());
    head.seq = seq;
    uint8_t raw[kHeadSize];
    packHead(head, raw);
    return fdSend(fd, raw, sizeof(raw)) && (body.empty() || fdSend(fd, body.data(), body.size()));
}

static void testBadClose(bronx::BxIoManager& iom, const char* tag, Kind kind,
                         const std::string& body, uint64_t seq) {
    std::string path = "/tmp/bronx_ipban_" + std::string(tag) + "_" + std::to_string(::getpid());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_CHECK(fd >= 0);
    if(fd < 0) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    ::unlink(path.c_str());
    TEST_CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    TEST_CHECK(::listen(fd, 1) == 0);

    std::atomic<bool> peerDone{false};
    std::atomic<bool> peerOk{false};
    std::thread peer([&]() {
        int conn = -1;
        uint64_t due = bronx::GetCurrentMs() + 2000;
        while(conn < 0 && bronx::GetCurrentMs() < due) {
            conn = ::accept(fd, nullptr, nullptr);
            if(conn >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
            usleep(10 * 1000);
        }
        Msg hello;
        bool ok = conn >= 0 && fdRecvMsg(conn, hello) && hello.kind == Kind::HELLO && hello.seq;
        if(ok) ok = fdSendMsg(conn, Kind::READY, readyToJson(1, 0), hello.seq);
        if(ok) ok = fdSendMsg(conn, kind, body, seq);
        if(conn >= 0) ::close(conn);
        ::close(fd);
        peerOk.store(ok, std::memory_order_release);
        peerDone.store(true, std::memory_order_release);
    });

    auto start = bronx::gateway::GatewayMetrics::instance().snapshot();
    auto before = start.ipbanMsgErrors;
    auto guard = std::make_shared<Guard>();
    SyncOpts opts;
    opts.subscribePath = path;
    opts.instanceId = tag;
    opts.minBackoffMs = 500;
    opts.maxBackoffMs = 500;
    opts.recvTimeoutMs = 1000;
    opts.stopTimeoutMs = 1000;
    opts.expiryTickMs = 0;
    auto sync = std::make_shared<SyncClient>(guard, opts);
    sync->start(&iom);

    uint64_t due = bronx::GetCurrentMs() + 1500;
    while(!peerDone.load(std::memory_order_acquire) && bronx::GetCurrentMs() < due) usleep(10 * 1000);
    TEST_CHECK(peerDone.load(std::memory_order_acquire));
    TEST_CHECK(peerOk.load(std::memory_order_acquire));
    while(bronx::gateway::GatewayMetrics::instance().snapshot().ipbanMsgErrors == before
          && bronx::GetCurrentMs() < due) usleep(10 * 1000);
    TEST_CHECK(bronx::gateway::GatewayMetrics::instance().snapshot().ipbanMsgErrors > before);
    auto after = bronx::gateway::GatewayMetrics::instance().snapshot();
    if(kind == Kind::SNAP) {
        TEST_CHECK_EQ(after.syncApply[1], start.syncApply[1] + 1);
        TEST_CHECK_EQ(after.syncResync[0], start.syncResync[0] + 1);
    } else if(kind == Kind::DELTA) {
        TEST_CHECK_EQ(after.syncApply[3], start.syncApply[3] + 1);
        TEST_CHECK_EQ(after.syncResync[2], start.syncResync[2] + 1);
    }
    sync->stop();
    TEST_CHECK(sync->waitStop(1000));
    peer.join();
    ::unlink(path.c_str());
}

static void testGoodApply(bronx::BxIoManager& iom, bool delta) {
    std::string tag = delta ? "gooddelta" : "goodsnap";
    std::string path = "/tmp/bronx_ipban_" + tag + "_" + std::to_string(::getpid());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_CHECK(fd >= 0);
    if(fd < 0) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    ::unlink(path.c_str());
    TEST_CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    TEST_CHECK(::listen(fd, 1) == 0);

    uint64_t epoch = delta ? 1 : 10;
    uint64_t target = delta ? 2 : 1;
    std::string body = delta ? deltaToJson(epoch, 1, 2, {})
                             : snapToJson(epoch, 1, {});
    std::atomic<bool> peerDone{false};
    std::atomic<bool> peerOk{false};
    std::thread peer([&]() {
        int conn = -1;
        uint64_t end = bronx::GetCurrentMs() + 2000;
        while(conn < 0 && bronx::GetCurrentMs() < end) {
            conn = ::accept(fd, nullptr, nullptr);
            if(conn >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
            usleep(10 * 1000);
        }
        Msg hello;
        bool ok = conn >= 0 && fdRecvMsg(conn, hello) && hello.kind == Kind::HELLO;
        if(ok) ok = fdSendMsg(conn, Kind::READY, readyToJson(epoch, target), hello.seq);
        if(ok) ok = fdSendMsg(conn, delta ? Kind::DELTA : Kind::SNAP, body, 8);
        Msg ackMsg;
        if(ok) ok = fdRecvMsg(conn, ackMsg) && ackMsg.kind == Kind::ACK && ackMsg.seq == 8;
        if(conn >= 0) ::close(conn);
        ::close(fd);
        peerOk.store(ok, std::memory_order_release);
        peerDone.store(true, std::memory_order_release);
    });

    auto guard = std::make_shared<Guard>();
    TEST_CHECK(guard->applyRemote(delta ? 1 : 9, 1, {}));
    SyncOpts opts;
    opts.subscribePath = path;
    opts.instanceId = tag;
    opts.minBackoffMs = 500;
    opts.maxBackoffMs = 500;
    opts.recvTimeoutMs = 1000;
    opts.stopTimeoutMs = 1000;
    opts.expiryTickMs = 0;
    auto before = bronx::gateway::GatewayMetrics::instance().snapshot();
    auto sync = std::make_shared<SyncClient>(guard, opts);
    sync->start(&iom);

    uint64_t due = bronx::GetCurrentMs() + 2000;
    while(!peerDone.load(std::memory_order_acquire) && bronx::GetCurrentMs() < due)
        usleep(10 * 1000);
    TEST_CHECK(peerDone.load(std::memory_order_acquire));
    TEST_CHECK(peerOk.load(std::memory_order_acquire));
    TEST_CHECK_EQ(guard->remoteVersion(), target);
    auto after = bronx::gateway::GatewayMetrics::instance().snapshot();
    size_t slot = delta ? 2 : 0;
    TEST_CHECK_EQ(after.syncApply[slot], before.syncApply[slot] + 1);
    if(!delta) TEST_CHECK_EQ(after.epochChanges, before.epochChanges + 1);
    sync->stop();
    TEST_CHECK(sync->waitStop(1000));
    peer.join();
    ::unlink(path.c_str());
}

int main() {
    std::string base = "/tmp/bronx_ipban_resync_" + std::to_string(::getpid());
    std::string submitPath = base + "_submit.sock";
    std::string subPath = base + "_sub.sock";

    DaemonOpts opts;
    opts.submitPath = submitPath;
    opts.subscribePath = subPath;
    opts.expiryTickMs = 200;
    opts.ackTimeoutMs = 200;
    opts.stopTimeoutMs = 1000;

    bronx::BxIoManager iomD(1, "daemon");
    bronx::BxIoManager iomG(1, "gw");
    bronx::BxIoManager iomP(1, "peer");
    auto daemon = std::make_shared<Daemon>(&iomD, opts);
    std::atomic<bool> done{false};

    iomD.post([&]() {
        if(!daemon->start()) {
            TEST_FAIL("daemon start failed");
            done.store(true);
        }
    });

    iomG.post([&]() {
        usleep(300 * 1000);

        auto first = dial(subPath, "gw-first");
        TEST_CHECK(first != nullptr);
        if(!first) {
            done.store(true);
            return;
        }
        uint64_t epoch = 0;
        uint64_t ver = 0;
        TEST_CHECK(readReady(first, epoch, ver));
        Msg snap;
        uint64_t snapEpoch = 0;
        uint64_t snapVer = 0;
        TEST_CHECK(readSnap(first, snap, snapEpoch, snapVer));
        TEST_CHECK_EQ(snapEpoch, epoch);
        TEST_CHECK_EQ(snapVer, ver);

        TEST_CHECK(sendMsg(first, Kind::SNAP_REQ, ""));
        Msg full;
        TEST_CHECK(readSnap(first, full, snapEpoch, snapVer));
        TEST_CHECK(ack(first, full, "gw-first", snapEpoch, snapVer));
        first->close();

        auto same = dial(subPath, "gw-same", epoch, ver);
        TEST_CHECK(same != nullptr);
        uint64_t sameEpoch = 0;
        uint64_t sameVer = 0;
        TEST_CHECK(readReady(same, sameEpoch, sameVer));
        TEST_CHECK_EQ(sameEpoch, epoch);
        TEST_CHECK_EQ(sameVer, ver);
        Msg ping;
        TEST_CHECK(recvMsg(same, ping) == MsgRet::OK);
        TEST_CHECK(ping.kind == Kind::PING && ping.seq);
        TEST_CHECK(sendMsg(same, Kind::PONG, "", ping.seq));
        same->close();

        // dirty 路径: SYNC 等 ACK 期间 engine 版本变化, ACK 到达后应自动推 SNAP/DELTA
        auto dirty = dial(subPath, "gw-dirty");
        TEST_CHECK(dirty != nullptr);
        if(dirty) {
            uint64_t dEpoch = 0, dVer = 0;
            TEST_CHECK(readReady(dirty, dEpoch, dVer));
            Msg dSnap; uint64_t dSnapEpoch = 0, dSnapVer = 0;
            TEST_CHECK(readSnap(dirty, dSnap, dSnapEpoch, dSnapVer));
            // ACK 前提交一条 risk, engine 版本 +1, session 标 dirty
            TEST_CHECK(submitRisk(submitPath, "dirty-1", "7.7.7.7"));
            usleep(150 * 1000);  // 等 engine 处理
            TEST_CHECK(ack(dirty, dSnap, "gw-dirty", dSnapEpoch, dSnapVer));
            // dirty → session 应再推一次
            Msg update; uint64_t updVer = 0;
            TEST_CHECK(readPush(dirty, update, updVer));
            TEST_CHECK(update.seq != 0);
            TEST_CHECK_MSG(updVer > dSnapVer, "dirty 后推送版本应前进");
            TEST_CHECK(ack(dirty, update, "gw-dirty", dSnapEpoch, updVer));
            dirty->close();
        }

        auto noAck = dial(subPath, "gw-no-ack");
        TEST_CHECK(noAck != nullptr);
        TEST_CHECK(readReady(noAck, sameEpoch, sameVer));
        TEST_CHECK(readSnap(noAck, snap, snapEpoch, snapVer));
        TEST_CHECK(readErrClose(noAck, "ack_timeout"));
        noAck->close();

        auto noPong = dial(subPath, "gw-no-pong");
        TEST_CHECK(noPong != nullptr);
        TEST_CHECK(readReady(noPong, sameEpoch, sameVer));
        TEST_CHECK(readSnap(noPong, snap, snapEpoch, snapVer));
        TEST_CHECK(ack(noPong, snap, "gw-no-pong", snapEpoch, snapVer));
        TEST_CHECK(recvMsg(noPong, ping) == MsgRet::OK);
        TEST_CHECK(ping.kind == Kind::PING && ping.seq);
        TEST_CHECK(readErrClose(noPong, "pong_timeout"));
        noPong->close();

        auto badPong = dial(subPath, "gw-bad-pong");
        TEST_CHECK(badPong != nullptr);
        TEST_CHECK(readReady(badPong, sameEpoch, sameVer));
        TEST_CHECK(readSnap(badPong, snap, snapEpoch, snapVer));
        TEST_CHECK(ack(badPong, snap, "gw-bad-pong", snapEpoch, snapVer));
        TEST_CHECK(recvMsg(badPong, ping) == MsgRet::OK);
        TEST_CHECK(ping.kind == Kind::PING && ping.seq);
        TEST_CHECK(sendMsg(badPong, Kind::PONG, "no", ping.seq));
        TEST_CHECK(readErrClose(badPong, "bad_pong"));
        badPong->close();

        auto badReq = dial(subPath, "gw-bad-req");
        TEST_CHECK(badReq != nullptr);
        TEST_CHECK(readReady(badReq, sameEpoch, sameVer));
        TEST_CHECK(readSnap(badReq, snap, snapEpoch, snapVer));
        TEST_CHECK(sendMsg(badReq, Kind::SNAP_REQ, "no", 1));
        TEST_CHECK(readErrClose(badReq, "bad_state"));
        badReq->close();

        auto badAck = dial(subPath, "gw-bad-ack");
        TEST_CHECK(badAck != nullptr);
        TEST_CHECK(readReady(badAck, sameEpoch, sameVer));
        TEST_CHECK(readSnap(badAck, snap, snapEpoch, snapVer));
        TEST_CHECK(ack(badAck, snap, "other", snapEpoch, snapVer));
        TEST_CHECK(readErrClose(badAck, "bad_ack"));
        badAck->close();

        uint64_t statDue = bronx::GetCurrentMs() + 500;
        while(daemon->stats().closes < 5 && bronx::GetCurrentMs() < statDue) usleep(10 * 1000);
        LinkStat stat = daemon->stats();
        TEST_CHECK(stat.ackTimeout >= 1);
        TEST_CHECK(stat.pongTimeout >= 1);
        TEST_CHECK(stat.msgErr >= 1);

        TEST_CHECK(badFrame(subPath, 0));
        TEST_CHECK(badFrame(subPath, 1));
        TEST_CHECK(badFrame(subPath, 2));
        TEST_CHECK(badFrame(subPath, 3));
        uint64_t frameDue = bronx::GetCurrentMs() + 500;
        while(daemon->stats().frameErr < 4 && bronx::GetCurrentMs() < frameDue) usleep(10 * 1000);
        TEST_CHECK(daemon->stats().frameErr >= 4);

        auto idle = bronx::BxSocket::MakeUnixTcpSocket();
        auto addr = bronx::BxUnixAddress::Create(subPath);
        bool idleOk = idle && addr && idle->connect(addr, 2000);
        TEST_CHECK(idleOk);
        if(!idleOk) {
            done.store(true);
            return;
        }
        auto idleSubmit = bronx::BxSocket::MakeUnixTcpSocket();
        auto submitAddr = bronx::BxUnixAddress::Create(submitPath);
        bool submitOk = idleSubmit && submitAddr && idleSubmit->connect(submitAddr, 2000);
        TEST_CHECK(submitOk);
        if(!submitOk) {
            done.store(true);
            return;
        }
        idleSubmit->setRecvTimeout(2000);
        idle->setRecvTimeout(1000);
        usleep(50 * 1000);
        daemon->stop();
        char ch = 0;
        TEST_CHECK(idle->recv(&ch, 1) == 0);
        TEST_CHECK(idleSubmit->recv(&ch, 1) == 0);
        TEST_CHECK(daemon->waitStop(100));
        TEST_CHECK(daemon->stats().forced >= 1);
        idle->close();
        idleSubmit->close();
        testBadClose(iomP, "badbye", Kind::BYE, "", 1);
        testBadClose(iomP, "baderr", Kind::ERR, errToJson("bad_ack"), 1);
        testBadClose(iomP, "badsnap", Kind::SNAP, "{}", 1);
        testBadClose(iomP, "baddelta", Kind::DELTA, deltaToJson(1, 9, 10, {}), 1);
        testGoodApply(iomP, false);
        testGoodApply(iomP, true);
        done.store(true);
    });

    uint64_t due = bronx::GetCurrentMs() + 10000;
    while(!done.load() && bronx::GetCurrentMs() < due) usleep(50 * 1000);
    if(!done.load()) TEST_FAIL("resync timed out");

    daemon->stop();
    iomD.stop();
    iomG.stop();
    iomP.stop();
    ::unlink(submitPath.c_str());
    ::unlink(subPath.c_str());
    return TEST_SUMMARY();
}
