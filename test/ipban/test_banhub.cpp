#include "test_util.h"
#include "endpoint.h"
#include "net_socket.h"
#include "proto.h"
#include "wire.h"
#include "engine.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace bronx::ipban;

struct Run {
    pid_t pid = -1;
    std::string log;
    uint16_t port = 0;
};

static std::string binDir() {
    char path[4096];
    ssize_t n = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if(n <= 0) return ".";
    path[n] = 0;
    std::string out(path);
    size_t slash = out.rfind('/');
    return slash == std::string::npos ? "." : out.substr(0, slash);
}

static bool waitSock(const std::string& path, uint64_t ms) {
    uint64_t end = bronx_test::now_ms() + ms;
    struct stat st{};
    while(bronx_test::now_ms() < end) {
        if(::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) return true;
        usleep(10 * 1000);
    }
    return ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

static uint16_t freePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if(::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0
       || ::getsockname(fd, (sockaddr*)&addr, &len) != 0) {
        ::close(fd);
        return 0;
    }
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

static Run startHub(const std::string& base, const std::string& db, uint16_t port = 0) {
    Run out;
    out.log = base + ".log";
    out.port = port ? port : freePort();
    std::string in = base + "_in.sock";
    std::string sub = base + "_sub.sock";
    std::string adm = base + "_adm.sock";
    std::string http = "127.0.0.1:" + std::to_string(out.port);
    out.pid = ::fork();
    if(out.pid != 0) return out;
    int fd = ::open(out.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
    }
    std::string exe = binDir() + "/banhub";
    ::execl(exe.c_str(), exe.c_str(), in.c_str(), sub.c_str(), db.c_str(),
            adm.c_str(), http.c_str(), (char*)nullptr);
    _exit(127);
}

static bool waitDone(const Run& run, int& code, uint64_t ms = 5000) {
    uint64_t end = bronx_test::now_ms() + ms;
    int status = 0;
    while(bronx_test::now_ms() < end) {
        pid_t got = ::waitpid(run.pid, &status, WNOHANG);
        if(got == run.pid) {
            code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return true;
        }
        if(got < 0 && errno != EINTR) return false;
        usleep(10 * 1000);
    }
    return false;
}

static bool stopHub(const Run& run, uint64_t ms = 8000) {
    if(run.pid <= 0 || ::kill(run.pid, SIGTERM) != 0) return false;
    uint64_t end = bronx_test::now_ms() + ms;
    int status = 0;
    while(bronx_test::now_ms() < end) {
        pid_t got = ::waitpid(run.pid, &status, WNOHANG);
        if(got == run.pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if(got < 0 && errno != EINTR) return false;
        usleep(10 * 1000);
    }
    ::kill(run.pid, SIGKILL);
    ::waitpid(run.pid, &status, 0);
    return false;
}

static bool hasLog(const std::string& path, const std::string& want) {
    std::ifstream in(path);
    std::string all((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return all.find(want) != std::string::npos;
}

static bool sendFull(int fd, const void* data, size_t len);

static std::string get(uint16_t port, const char* path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }
    std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if(!sendFull(fd, req.data(), req.size())) {
        ::close(fd);
        return {};
    }
    std::string out;
    char buf[2048];
    while(true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;
        out.append(buf, n);
    }
    ::close(fd);
    return out;
}

static std::string waitGet(uint16_t port, const char* path, uint64_t ms = 5000) {
    uint64_t end = bronx_test::now_ms() + ms;
    std::string out;
    while(bronx_test::now_ms() < end) {
        out = get(port, path);
        if(!out.empty()) return out;
        usleep(10 * 1000);
    }
    return out;
}

static int dial(const std::string& path) {
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

static bool sendFull(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while(len) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool readFull(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while(len) {
        ssize_t n = ::recv(fd, p, len, 0);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool call(const std::string& path, Kind kind, const std::string& body,
                 CtlRes& out) {
    int fd = dial(path);
    if(fd < 0) return false;
    Head head;
    head.kind = static_cast<uint16_t>(kind);
    head.bodyLen = static_cast<uint32_t>(body.size());
    head.seq = 7;
    uint8_t raw[kHeadSize];
    packHead(head, raw);
    bool ok = sendFull(fd, raw, sizeof(raw))
        && sendFull(fd, body.data(), body.size())
        && readFull(fd, raw, sizeof(raw)) && unpackHead(raw, head)
        && head.kind == static_cast<uint16_t>(Kind::RESULT) && head.seq == 7
        && head.bodyLen <= kMaxBody;
    std::string reply;
    if(ok) {
        reply.resize(head.bodyLen);
        ok = readFull(fd, reply.data(), reply.size()) && ctlResFromJson(reply, out);
    }
    ::close(fd);
    return ok;
}

static bool risk(const std::string& path, const Risk& risk) {
    int fd = dial(path);
    if(fd < 0) return false;
    std::string body = riskToJson(risk);
    Head head;
    head.kind = static_cast<uint16_t>(Kind::RISK);
    head.bodyLen = static_cast<uint32_t>(body.size());
    uint8_t raw[kHeadSize];
    packHead(head, raw);
    bool ok = sendFull(fd, raw, sizeof(raw)) && sendFull(fd, body.data(), body.size());
    ::close(fd);
    return ok;
}

static void clean(const std::string& base, const std::string& db) {
    ::unlink((base + "_in.sock").c_str());
    ::unlink((base + "_sub.sock").c_str());
    ::unlink((base + "_adm.sock").c_str());
    ::unlink((base + ".log").c_str());
    ::unlink(db.c_str());
    ::unlink((db + "-wal").c_str());
    ::unlink((db + "-shm").c_str());
}

static void testStop() {
    std::string base = "/tmp/banhub_stop_" + std::to_string(::getpid());
    std::string db = base + ".db";
    clean(base, db);
    Run run = startHub(base, db);
    TEST_CHECK(run.pid > 0);
    TEST_CHECK(waitSock(base + "_adm.sock", 5000));
    std::string health = waitGet(run.port, "/healthz");
    TEST_CHECK(health.find("200 OK") != std::string::npos);

    CtlPut put;
    TEST_CHECK(parseCidr("203.0.113.7", put.ip));
    put.action = Act::DENY;
    put.reason = "test";
    CtlRes res;
    TEST_CHECK(call(base + "_adm.sock", Kind::PUT, ctlPutToJson(put), res));
    TEST_CHECK(res.ok && res.changed);

    Risk hit;
    hit.id = "hub-risk";
    TEST_CHECK(parseCidr("198.51.100.8", hit.ip));
    hit.src = Src::WAF;
    hit.type = RiskType::INJECT;
    hit.atMs = bronx_test::now_ms();
    TEST_CHECK(risk(base + "_in.sock", hit));
    TEST_CHECK(risk(base + "_in.sock", hit));
    usleep(100 * 1000);
    std::string body = get(run.port, "/metrics");
    TEST_CHECK(body.find("ipban_hub_rules 2\n") != std::string::npos);
    TEST_CHECK(body.find("ipban_hub_risks_total{result=\"ok\",source=\"waf\"} 1\n") != std::string::npos);
    TEST_CHECK(body.find("ipban_hub_risks_total{result=\"ignored\",source=\"waf\"} 1\n") != std::string::npos);
    TEST_CHECK(body.find("ipban_hub_changes_total{op=\"put\",source=\"admin\"} 1\n") != std::string::npos);
    TEST_CHECK(body.find("ipban_hub_db_writes_total{op=\"put\",result=\"ok\"} 2\n") != std::string::npos);
    TEST_CHECK(body.find("ipban_hub_db_write_seconds_bucket{le=\"+Inf\"} 2\n") != std::string::npos);
    TEST_CHECK(body.find("ipban_hub_db_write_seconds_count 2\n") != std::string::npos);
    TEST_CHECK(body.find("ipban_hub_db_write_seconds_sum ") != std::string::npos);
    TEST_CHECK(get(run.port, "/nope").find("404 Not Found") != std::string::npos);

    int idle = dial(base + "_sub.sock");
    TEST_CHECK(idle >= 0);
    TEST_CHECK(stopHub(run));
    if(idle >= 0) ::close(idle);
    TEST_CHECK(hasLog(run.log, "ipban stopped"));
    TEST_CHECK(hasLog(run.log, "pending db ops") == false);
    TEST_CHECK(::access((base + "_in.sock").c_str(), F_OK) != 0);
    TEST_CHECK(::access((base + "_sub.sock").c_str(), F_OK) != 0);
    TEST_CHECK(::access((base + "_adm.sock").c_str(), F_OK) != 0);

    Run again = startHub(base, db);
    TEST_CHECK(waitSock(base + "_adm.sock", 5000));
    TEST_CHECK(call(base + "_adm.sock", Kind::LIST, "", res));
    TEST_CHECK(res.ok && res.rules.size() == 2);
    TEST_CHECK(stopHub(again));
    clean(base, db);
}

class BadDb : public Db {
public:
    bool badWrite = true;
    bool badRead = false;

    bool save(const Rule&) override { return !badWrite; }
    bool del(const std::string&, uint64_t) override { return !badWrite; }
    bool load(uint64_t, std::vector<Rule>& out, uint64_t& ver) override {
        out.clear();
        ver = 0;
        return !badRead;
    }
};

static void testDbStat() {
    auto db = std::make_shared<BadDb>();
    PolicyEngine engine;
    engine.setDb(db, nullptr, 0);
    Risk in;
    in.id = "db";
    TEST_CHECK(parseCidr("192.0.2.9", in.ip));
    in.atMs = bronx_test::now_ms();
    TEST_CHECK(engine.onRisk(in));
    HubStat stat = engine.hubStat();
    TEST_CHECK(stat.db.enabled && !stat.db.ok && stat.db.pending == 1);
    TEST_CHECK(stat.db.writes[2] == 1);
    db->badWrite = false;
    engine.tickExpiry();
    stat = engine.hubStat();
    TEST_CHECK(stat.db.ok && stat.db.pending == 0);
    TEST_CHECK(stat.db.writes[0] == 1 && stat.db.timeCount == 2);

    auto broken = std::make_shared<BadDb>();
    broken->badRead = true;
    PolicyEngine restore;
    restore.setDb(broken, nullptr);
    TEST_CHECK(!restore.restore());
    stat = restore.hubStat();
    TEST_CHECK(!stat.db.ok && stat.db.restore[2] == 1);
}

static void testBusyPort() {
    std::string root = "/tmp/banhub_busy_" + std::to_string(::getpid());
    std::string one = root + "_one";
    std::string two = root + "_two";
    std::string db1 = one + ".db";
    std::string db2 = two + ".db";
    clean(one, db1);
    clean(two, db2);
    Run first = startHub(one, db1);
    TEST_CHECK(!waitGet(first.port, "/healthz").empty());
    Run second = startHub(two, db2, first.port);
    int code = 0;
    TEST_CHECK(waitDone(second, code));
    TEST_CHECK(code != 0);
    TEST_CHECK(::access((two + "_in.sock").c_str(), F_OK) != 0);
    TEST_CHECK(::access((two + "_sub.sock").c_str(), F_OK) != 0);
    TEST_CHECK(::access((two + "_adm.sock").c_str(), F_OK) != 0);
    TEST_CHECK(stopHub(first));
    clean(one, db1);
    clean(two, db2);
}

static void testRepeat() {
    std::string root = "/tmp/banhub_loop_" + std::to_string(::getpid());
    for(int i = 0; i < 20; ++i) {
        std::string base = root + "_" + std::to_string(i);
        std::string db = base + ".db";
        clean(base, db);
        Run run = startHub(base, db);
        TEST_CHECK(waitSock(base + "_adm.sock", 5000));
        TEST_CHECK(stopHub(run));
        TEST_CHECK(hasLog(run.log, "ipban stopped"));
        clean(base, db);
    }
}

int main() {
    testStop();
    testDbStat();
    testBusyPort();
    testRepeat();
    return TEST_SUMMARY();
}
