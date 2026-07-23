// admin 手动改 IP 名单的命令行. 连 daemon 的 admin UDS 口, 发一条 PUT/DEL/LIST 收 RESULT.
// 只碰客户端: 不动 wire 协议也不动 daemon. 面向人用, 顺带留 --json 给脚本.
#include "proto.h"
#include "wire.h"
#include "judge.h"    // ruleIdFor, unban 按 ip 算规则 id
#include "rule.h"
#include "ip.h"
#include "util.h"     // bronx::GetCurrentMs, 算剩余 ttl
#include "reactor.h"
#include "endpoint.h"
#include "net_socket.h"
#include "sync.h"
#include "log.h"     // 压掉框架 system 日志, CLI 自己走 cout/cerr
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace bronx::ipban;

// 退出码分级, 让脚本能判断到底哪一步挂了.
enum class Rc { OK = 0, USAGE = 1, DENIED = 2, UNREACH = 3, PROTO = 4 };

struct Cmd {
    Kind kind = Kind::LIST;
    std::string body;
    std::string sock = "/tmp/bronx_ip_admin.sock";
    bool json = false;          // --json: 出原始 CtlRes JSON, 不做人类排版
    int64_t timeoutMs = 3000;   // connect + recv 各自的上限
    // 下面几个只给成功回显用
    Act action = Act::DENY;
    std::string ipText;
    uint64_t ttlMs = 0;
    std::string id;
    std::string rawBody;        // daemon 回来的原始 body, --json 直接吐
};

static void usage() {
    std::cerr
        << "banctl - bronx ip 名单控制\n"
        << "用法:\n"
        << "  banctl [选项] allow <cidr> [ttl] [reason]   放行(白名单)\n"
        << "  banctl [选项] deny  <cidr> [ttl] [reason]   封禁(黑名单)\n"
        << "  banctl [选项] unban <cidr>                  按 ip 解封(删 admin 规则)\n"
        << "  banctl [选项] del   <rule_id>               按规则 id 删(如 admin:1.2.3.4)\n"
        << "  banctl [选项] list                          列当前所有规则\n"
        << "选项:\n"
        << "  --sock <path>    admin socket 路径(默认 $BRONX_IP_ADMIN_SOCK 或 /tmp/bronx_ip_admin.sock)\n"
        << "  --timeout <ms>   连接/收发上限, 默认 3000\n"
        << "  --json           输出原始 JSON(供脚本), 不做人类排版\n"
        << "  -h, --help       本帮助\n"
        << "ttl 形如 30s / 10m / 2h / 7d, 纯数字按毫秒, perm/0 为永久. 缺省永久.\n";
}

// 解析无符号整数, 拒负号和尾巴垃圾字符.
static bool parseU64(const char* s, uint64_t& out) {
    if(!s || !*s || *s == '-') return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s, &end, 10);
    if(errno || !end || *end) return false;
    out = (uint64_t)v;
    return true;
}

// ttl 文本 -> 毫秒. 纯数字=毫秒(向后兼容); 带 s/m/h/d 后缀按单位; perm/0=永久(0).
static bool parseTtl(const std::string& s, uint64_t& outMs) {
    if(s.empty()) return false;
    if(s == "perm" || s == "0") { outMs = 0; return true; }
    char suf = s.back();
    uint64_t mul = 0;
    switch(suf) {
        case 's': mul = 1000ull; break;
        case 'm': mul = 60ull * 1000; break;
        case 'h': mul = 3600ull * 1000; break;
        case 'd': mul = 86400ull * 1000; break;
        default: break;
    }
    uint64_t n = 0;
    if(mul == 0) { if(!parseU64(s.c_str(), n)) return false; outMs = n; return true; }
    if(!parseU64(s.substr(0, s.size() - 1).c_str(), n)) return false;
    if(n != 0 && n > (uint64_t)-1 / mul) return false;   // 溢出防护
    outMs = n * mul;
    return true;
}

// 毫秒时长 -> 紧凑人话, 如 2h9m / 45s / 3d4h.
static std::string humanDur(uint64_t ms) {
    if(ms == 0) return "0s";
    uint64_t sec = ms / 1000;
    uint64_t d = sec / 86400; sec %= 86400;
    uint64_t h = sec / 3600;  sec %= 3600;
    uint64_t m = sec / 60;    sec %= 60;
    std::ostringstream os;
    int parts = 0;
    if(d && parts < 2) { os << d << "d"; ++parts; }
    if(h && parts < 2) { os << h << "h"; ++parts; }
    if(m && parts < 2) { os << m << "m"; ++parts; }
    if(sec && parts < 2) { os << sec << "s"; ++parts; }
    if(!parts) os << "<1s";
    return os.str();
}

// 规则的过期列: 永久 or 剩余时长(已过期标 expired).
static std::string expireCol(const Rule& r, uint64_t now) {
    if(r.expireAtMs == 0) return "perm";
    if(now >= r.expireAtMs) return "expired";
    return humanDur(r.expireAtMs - now);
}

// list 的人类可读表. 列: action ip src pri ttl reason  (id 放最后, 可能长).
static void renderList(const CtlRes& res) {
    if(res.rules.empty()) {
        std::cout << "no rules (version=" << res.version << ")\n";
        return;
    }
    uint64_t now = bronx::GetCurrentMs();
    // 先量各列宽再对齐, 免得 v6 长地址把表撑歪.
    size_t wIp = 2, wSrc = 3, wTtl = 3, wReason = 6;
    for(const auto& r : res.rules) {
        wIp     = std::max(wIp, r.ip.toString().size());
        wSrc    = std::max(wSrc, std::strlen(srcName(r.src)));
        wTtl    = std::max(wTtl, expireCol(r, now).size());
        wReason = std::max(wReason, r.reason.size());
    }
    std::cout << std::left
              << std::setw(6) << "ACT" << "  "
              << std::setw((int)wIp) << "IP" << "  "
              << std::setw((int)wSrc) << "SRC" << "  "
              << std::setw(4) << "PRI" << "  "
              << std::setw((int)wTtl) << "TTL" << "  "
              << std::setw((int)wReason) << "REASON" << "  "
              << "ID" << "\n";
    for(const auto& r : res.rules) {
        std::cout << std::left
                  << std::setw(6) << actName(r.action) << "  "
                  << std::setw((int)wIp) << r.ip.toString() << "  "
                  << std::setw((int)wSrc) << srcName(r.src) << "  "
                  << std::setw(4) << r.priority << "  "
                  << std::setw((int)wTtl) << expireCol(r, now) << "  "
                  << std::setw((int)wReason) << r.reason << "  "
                  << r.id << "\n";
    }
    std::cout << res.rules.size() << " rule(s), version=" << res.version << "\n";
}

// put/del 成功后的一行人话. 区分改了/没改(幂等)/没找到.
static void renderEdit(const Cmd& cmd, const CtlRes& res) {
    if(cmd.kind == Kind::PUT) {
        std::string ttl = cmd.ttlMs == 0 ? "perm" : humanDur(cmd.ttlMs);
        if(res.changed)
            std::cout << actName(cmd.action) << " " << cmd.ipText << " ttl=" << ttl
                      << " id=" << res.id << " version=" << res.version << "\n";
        else
            std::cout << "no change (" << actName(cmd.action) << " " << cmd.ipText
                      << " already in effect) version=" << res.version << "\n";
    } else {   // DEL
        if(res.changed)
            std::cout << "removed " << res.id << " version=" << res.version << "\n";
        else
            std::cout << "not found: " << res.id << " (no change) version="
                      << res.version << "\n";
    }
}

// 一次请求-应答. 全在 iom 的协程里跑, 主线程 sem 等它.
// rc 出连接层结果, res 出 daemon 应答. rawBody 出原始 body 供 --json.
static void io(const Cmd& cmd, Rc& rc, CtlRes& res, std::string& rawBody, bool& gotRes) {
    auto sock = bronx::BxSocket::MakeUnixTcpSocket();
    auto addr = bronx::BxUnixAddress::Create(cmd.sock);
    if(!sock || !addr) { rc = Rc::UNREACH; return; }
    if(!sock->connect(addr, cmd.timeoutMs)) {
        rc = Rc::UNREACH;
        sock->close();
        return;
    }
    // daemon 收了连接却不回时别吊死, 给 recv 也套上上限.
    sock->setRecvTimeout(cmd.timeoutMs);
    if(!sendMsg(sock, cmd.kind, cmd.body, 1)) {
        rc = Rc::PROTO;
        sock->close();
        return;
    }
    Msg msg;
    if(recvMsg(sock, msg) != MsgRet::OK || msg.kind != Kind::RESULT || msg.seq != 1) {
        rc = Rc::PROTO;
        sock->close();
        return;
    }
    rawBody = msg.body;
    if(!ctlResFromJson(msg.body, res)) { rc = Rc::PROTO; sock->close(); return; }
    gotRes = true;
    rc = res.ok ? Rc::OK : Rc::DENIED;
    sock->close();
}

static Rc run(const Cmd& cmd) {
    Rc rc = Rc::UNREACH;
    CtlRes res;
    std::string rawBody;
    bool gotRes = false;
    bronx::BxIoManager iom(1, "banctl");
    bronx::BxSemaphore sem(0);
    iom.post([&]() {
        io(cmd, rc, res, rawBody, gotRes);
        sem.notify();
    });
    sem.wait();
    iom.stop();

    if(rc == Rc::UNREACH) {
        std::cerr << "banctl: 连不上 daemon (" << cmd.sock << ")\n";
        return rc;
    }
    if(rc == Rc::PROTO || !gotRes) {
        std::cerr << "banctl: daemon 无有效应答(超时或协议错)\n";
        return Rc::PROTO;
    }
    if(cmd.json) { std::cout << rawBody << "\n"; return rc; }
    if(rc == Rc::DENIED) {
        std::cerr << "banctl: daemon 拒绝: "
                  << (res.error.empty() ? "unknown" : res.error) << "\n";
        return rc;
    }
    if(cmd.kind == Kind::LIST) renderList(res);
    else renderEdit(cmd, res);
    return Rc::OK;
}

// 解析全局选项 + 子命令. 成功填 cmd 返回 true; help 和错误都返回 false.
static bool parse(int argc, char** argv, Cmd& cmd, bool& help) {
    if(const char* e = ::getenv("BRONX_IP_ADMIN_SOCK")) {
        if(*e) cmd.sock = e;
    }
    int i = 1;
    for(; i < argc; ++i) {
        std::string a = argv[i];
        if(a == "-h" || a == "--help") { help = true; return false; }
        if(a == "--json") { cmd.json = true; continue; }
        if(a == "--sock") {
            if(++i >= argc) return false;
            cmd.sock = argv[i];
            continue;
        }
        if(a == "--timeout") {
            uint64_t ms = 0;
            if(++i >= argc || !parseU64(argv[i], ms) || ms == 0 || ms > 600000) return false;
            cmd.timeoutMs = (int64_t)ms;
            continue;
        }
        break;   // 第一个非选项 = 子命令
    }
    if(i >= argc) return false;
    std::string op = argv[i++];

    if(op == "list") { cmd.kind = Kind::LIST; return i == argc; }

    if(op == "del") {
        if(i + 1 != argc) return false;
        std::string id = argv[i];
        if(id.empty() || id.size() > 256) return false;
        cmd.kind = Kind::DEL;
        cmd.id = id;
        cmd.body = ctlDelToJson(id);
        return true;
    }

    if(op == "unban") {   // 按 ip 解封: 算出 admin 规则 id 再 del
        if(i + 1 != argc) return false;
        Ip ip;
        if(!parseCidr(argv[i], ip)) return false;
        cmd.kind = Kind::DEL;
        cmd.id = ruleIdFor(Src::ADMIN, ip);
        cmd.body = ctlDelToJson(cmd.id);
        return true;
    }

    if(op != "allow" && op != "deny") return false;
    CtlPut put;
    if(i >= argc || !parseCidr(argv[i], put.ip)) return false;
    cmd.ipText = put.ip.toString();
    ++i;
    put.action = op == "allow" ? Act::ALLOW : Act::DENY;
    if(i < argc) {   // ttl 位置固定在 cidr 之后, 给了就必须解析得出, 解析失败即报错
        if(!parseTtl(argv[i], put.ttlMs)) return false;
        ++i;
    }
    put.reason = i < argc ? argv[i++] : "admin";
    if(i != argc || put.reason.size() > 256) return false;
    cmd.kind = Kind::PUT;
    cmd.action = put.action;
    cmd.ttlMs = put.ttlMs;
    cmd.body = ctlPutToJson(put);
    return true;
}

int main(int argc, char** argv) {
    // 框架 socket/hook 会往 system logger 吐 ERROR(如 connect 失败), CLI 不想让它污染 stderr.
    bronx::LoggerMgr::GetInstance()->getRoot()->setLevel(bronx::BxLogLevel::FATAL);
    bronx::LoggerMgr::GetInstance()->getLogger("system")->setLevel(bronx::BxLogLevel::FATAL);
    Cmd cmd;
    bool help = false;
    if(!parse(argc, argv, cmd, help)) {
        usage();
        return (int)(help ? Rc::OK : Rc::USAGE);
    }
    return (int)run(cmd);
}
