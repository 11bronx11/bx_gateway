#include "test_util.h"
#include "daemon.h"
#include "db.h"
#include "engine.h"
#include "guard.h"
#include "proto.h"
#include "pull.h"
#include "report.h"
#include "snap.h"
#include "util.h"
#include "wire.h"
#include "endpoint.h"
#include "reactor.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string>
#include <unistd.h>

using namespace bronx::ipban;

static Ip ip(const char* s) {
    Ip out;
    parseCidr(s, out);
    return out;
}

static bool waitAct(const Guard::ptr& guard, const Ip& addr, Act want, uint64_t ms) {
    uint64_t end = bronx::GetCurrentMs() + ms;
    while(bronx::GetCurrentMs() < end) {
        if(guard->eval(addr).action == want) return true;
        usleep(20 * 1000);
    }
    return guard->eval(addr).action == want;
}

static bool call(const std::string& path, Kind kind, const std::string& body,
                 CtlRes& out) {
    auto sock = bronx::BxSocket::MakeUnixTcpSocket();
    auto addr = bronx::BxUnixAddress::Create(path);
    if(!sock || !addr || !sock->connect(addr, 2000)) return false;
    sock->setRecvTimeout(2000);
    if(!sendMsg(sock, kind, body, 7)) return false;
    Msg msg;
    bool ok = recvMsg(sock, msg) == MsgRet::OK
        && msg.kind == Kind::RESULT && msg.seq == 7
        && ctlResFromJson(msg.body, out);
    sock->close();
    return ok;
}

static bool waitSocket(const std::string& path, uint64_t ms) {
    uint64_t end = bronx::GetCurrentMs() + ms;
    struct stat st{};
    while(bronx::GetCurrentMs() < end) {
        if(::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) return true;
        usleep(10 * 1000);
    }
    return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

static int dialFd(const std::string& path) {
    sockaddr_un addr{};
    if(path.size() >= sizeof(addr.sun_path)) return -1;
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0) return -1;
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.data(), path.size());
    if(::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) return fd;
    ::close(fd);
    return -1;
}

static bool fdSend(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    while(len) {
        int n = ::send(fd, p, len, MSG_NOSIGNAL);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool fdRead(int fd, void* data, size_t len) {
    char* p = (char*)data;
    while(len) {
        int n = ::recv(fd, p, len, 0);
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
    msg.kind = (Kind)head.kind;
    msg.seq = head.seq;
    msg.body.resize(head.bodyLen);
    return !head.bodyLen || fdRead(fd, msg.body.data(), msg.body.size());
}

static void testEngine() {
    PolicyEngine engine;
    Risk risk;
    risk.id = "risk-1";
    risk.ip = ip("1.2.3.4");
    risk.src = Src::RATE;
    TEST_CHECK(engine.onRisk(risk));

    EditRes add = engine.setAdmin(risk.ip, Act::ALLOW, 0, "ops");
    TEST_CHECK(add.changed);
    TEST_CHECK_EQ(add.id, std::string("admin:1.2.3.4"));
    TEST_CHECK_EQ(add.version, 2u);

    auto snap = compileSnap(1, Act::ALLOW, engine.rules(), bronx::GetCurrentMs());
    TEST_CHECK(snap->eval(risk.ip, bronx::GetCurrentMs()).action == Act::ALLOW);

    EditRes same = engine.setAdmin(risk.ip, Act::ALLOW, 0, "ops");
    TEST_CHECK(!same.changed);
    TEST_CHECK_EQ(same.version, 2u);

    EditRes del = engine.remove(add.id);
    TEST_CHECK(del.changed);
    TEST_CHECK_EQ(del.version, 3u);
    snap = compileSnap(2, Act::ALLOW, engine.rules(), bronx::GetCurrentMs());
    TEST_CHECK(snap->eval(risk.ip, bronx::GetCurrentMs()).action == Act::DENY);

    EditRes huge = engine.setAdmin(ip("203.0.113.1"), Act::DENY,
        std::numeric_limits<uint64_t>::max(), "huge");
    TEST_CHECK(!huge.changed && huge.error == "ttl_too_large");

    PolicyEngine riskEngine;
    Risk hugeRisk;
    hugeRisk.id = "huge-risk";
    hugeRisk.ip = ip("203.0.113.2");
    hugeRisk.src = Src::RATE;
    hugeRisk.banMs = std::numeric_limits<uint64_t>::max();
    TEST_CHECK(riskEngine.onRisk(hugeRisk));
    auto hugeRules = riskEngine.rules();
    TEST_CHECK_EQ(hugeRules.size(), 1u);
    TEST_CHECK_EQ(hugeRules[0].expireAtMs,
                  (uint64_t)std::numeric_limits<int64_t>::max());
}

static void testDb() {
    std::string path = "/tmp/bronx_ipban_ctl_db_" + std::to_string(::getpid()) + ".db";
    ::unlink(path.c_str());
    auto db = SqliteDb::open(path);
    TEST_CHECK(db != nullptr);
    PolicyEngine engine;
    engine.setDb(db, nullptr, 600000);
    EditRes add = engine.setAdmin(ip("10.0.0.0/8"), Act::ALLOW, 1000, "short");
    std::vector<Rule> rules;
    uint64_t ver = 0;
    TEST_CHECK(db->load(bronx::GetCurrentMs(), rules, ver));
    TEST_CHECK_EQ(rules.size(), 1u);
    TEST_CHECK_EQ(rules[0].id, add.id);
    TEST_CHECK(engine.remove(add.id).changed);
    TEST_CHECK(db->load(bronx::GetCurrentMs(), rules, ver));
    TEST_CHECK(rules.empty());
    ::unlink(path.c_str());
    ::unlink((path + "-wal").c_str());
    ::unlink((path + "-shm").c_str());
}

static void testUds() {
    std::string base = "/tmp/bronx_ipban_ctl_" + std::to_string(::getpid());
    DaemonOpts dopts;
    dopts.submitPath = base + "_in.sock";
    dopts.subscribePath = base + "_sub.sock";
    dopts.adminPath = base + "_admin.sock";
    dopts.expiryTickMs = 50;

    bronx::BxIoManager iomD(2, "ctld");
    bronx::BxIoManager iomG(1, "ctlg");
    auto daemon = std::make_shared<Daemon>(&iomD, dopts);
    auto guard = std::make_shared<Guard>();
    SyncOpts sopts;
    sopts.subscribePath = dopts.subscribePath;
    sopts.instanceId = "ctl-gw";
    auto sync = std::make_shared<SyncClient>(guard, sopts);
    ReportOpts ropts;
    ropts.submitPath = dopts.submitPath;
    auto reporter = std::make_shared<Reporter>(ropts);
    std::atomic<bool> done{false};

    iomD.post([&]() {
        if(!daemon->start()) TEST_FAIL("daemon start");
    });
    TEST_CHECK(waitSocket(dopts.adminPath, 3000));
    struct stat st{};
    TEST_CHECK(::stat(dopts.adminPath.c_str(), &st) == 0);
    TEST_CHECK_EQ(st.st_mode & 0777, 0600u);
    iomG.post([&]() {
        usleep(100 * 1000);
        sync->start(&iomG);
        reporter->start(&iomG);
        uint64_t end = bronx::GetCurrentMs() + 3000;
        while(!guard->remoteEpoch() && bronx::GetCurrentMs() < end) usleep(20 * 1000);
        TEST_CHECK(guard->remoteEpoch() != 0);

        Ip bad = ip("1.2.3.4");
        Risk risk;
        risk.id = "ctl-risk";
        risk.ip = bad;
        risk.src = Src::RATE;
        risk.banMs = 60000;
        TEST_CHECK(reporter->tryReport(risk));
        TEST_CHECK(waitAct(guard, bad, Act::DENY, 3000));

        CtlPut put;
        put.ip = bad;
        put.action = Act::ALLOW;
        put.reason = "ops";
        CtlRes res;
        TEST_CHECK(call(dopts.adminPath, Kind::PUT, ctlPutToJson(put), res));
        TEST_CHECK(res.ok && res.changed);
        TEST_CHECK_EQ(res.id, std::string("admin:1.2.3.4"));
        TEST_CHECK(waitAct(guard, bad, Act::ALLOW, 3000));

        TEST_CHECK(call(dopts.adminPath, Kind::LIST, "", res));
        TEST_CHECK(res.ok);
        TEST_CHECK_EQ(res.rules.size(), 2u);

        TEST_CHECK(call(dopts.adminPath, Kind::DEL, ctlDelToJson("admin:1.2.3.4"), res));
        TEST_CHECK(res.ok && res.changed);
        TEST_CHECK(waitAct(guard, bad, Act::DENY, 3000));

        TEST_CHECK(call(dopts.adminPath, Kind::DEL, ctlDelToJson("rate:1.2.3.4"), res));
        TEST_CHECK(res.ok && res.changed);
        TEST_CHECK(waitAct(guard, bad, Act::ALLOW, 3000));

        TEST_CHECK(call(dopts.adminPath, Kind::PUT, "{}", res));
        TEST_CHECK(!res.ok && res.error == "bad_put");

        CtlPut huge;
        huge.ip = ip("203.0.113.3");
        huge.action = Act::DENY;
        huge.ttlMs = std::numeric_limits<uint64_t>::max();
        huge.reason = "huge";
        TEST_CHECK(call(dopts.adminPath, Kind::PUT, ctlPutToJson(huge), res));
        TEST_CHECK(!res.ok && !res.changed && res.error == "ttl_too_large");
        sync->stop();
        reporter->stop();
        done.store(true);
    });

    uint64_t end = bronx::GetCurrentMs() + 15000;
    while(!done.load() && bronx::GetCurrentMs() < end) usleep(20 * 1000);
    if(!done.load()) TEST_FAIL("ctl timeout");
    daemon->stop();
    iomD.stop();
    iomG.stop();
    ::unlink(dopts.submitPath.c_str());
    ::unlink(dopts.subscribePath.c_str());
    ::unlink(dopts.adminPath.c_str());
}

static void testStopRejectsAdmin() {
    std::string base = "/tmp/bronx_ipban_ctl_stop_" + std::to_string(::getpid());
    DaemonOpts opts;
    opts.submitPath = base + "_in.sock";
    opts.subscribePath = base + "_sub.sock";
    opts.adminPath = base + "_admin.sock";

    bronx::BxIoManager iom(3, "ctlstop");
    auto daemon = std::make_shared<Daemon>(&iom, opts);
    iom.post([&]() {
        if(!daemon->start()) TEST_FAIL("stop daemon start");
    });
    TEST_CHECK(waitSocket(opts.adminPath, 3000));

    int fd = dialFd(opts.adminPath);
    TEST_CHECK(fd >= 0);
    timeval timeout{2, 0};
    TEST_CHECK(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    CtlPut put;
    put.ip = ip("203.0.113.9");
    put.action = Act::DENY;
    put.reason = "stop";
    std::string body = ctlPutToJson(put);
    Head head;
    head.kind = (uint16_t)Kind::PUT;
    head.bodyLen = (uint32_t)body.size();
    head.seq = 9;
    uint8_t raw[kHeadSize];
    packHead(head, raw);
    TEST_CHECK(fdSend(fd, raw, sizeof(raw)));
    usleep(20 * 1000);

    uint64_t before = daemon->version();
    daemon->beginStop();
    TEST_CHECK(fdSend(fd, body.data(), body.size()));
    Msg msg;
    CtlRes res;
    TEST_CHECK(fdRecvMsg(fd, msg));
    TEST_CHECK(msg.kind == Kind::RESULT && msg.seq == 9);
    TEST_CHECK(ctlResFromJson(msg.body, res));
    TEST_CHECK(!res.ok && res.error == "stopping");
    TEST_CHECK_EQ(daemon->version(), before);

    ::close(fd);
    daemon->stop();
    iom.stop();
    ::unlink(opts.submitPath.c_str());
    ::unlink(opts.subscribePath.c_str());
    ::unlink(opts.adminPath.c_str());
}

int main() {
    testEngine();
    testDb();
    testUds();
    testStopRejectsAdmin();
    return TEST_SUMMARY();
}
