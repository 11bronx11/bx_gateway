#include "hub_http.h"
#include <algorithm>
#include <sstream>

namespace bronx {
namespace ipban {

static bool sendFull(const bronx::BxSocket::ptr& sock, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while(len) {
        int n = sock->send(p, len);
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static void reply(const bronx::BxSocket::ptr& sock, int code,
                  const char* text, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << code << " " << text << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n" << body;
    std::string raw = out.str();
    sendFull(sock, raw.data(), raw.size());
}

static std::string metrics(const HubStat& s) {
    static const char* src[] = {"waf", "rate", "biz", "admin", "static", "emerg"};
    static const char* result[] = {"ok", "ignored", "failed"};
    static const char* op[] = {"put", "del", "expire"};
    static const char* err[] = {"frame", "msg", "ack_timeout", "pong_timeout", "forced"};
    static const char* le[] = {
        "0.001", "0.005", "0.01", "0.025", "0.05",
        "0.1", "0.25", "0.5", "1", "+Inf"
    };
    std::ostringstream out;
    out << "# TYPE ipban_hub_up gauge\n"
        << "ipban_hub_up " << (s.up ? 1 : 0) << "\n"
        << "# TYPE ipban_hub_version gauge\n"
        << "ipban_hub_version " << s.version << "\n"
        << "# TYPE ipban_hub_rules gauge\n"
        << "ipban_hub_rules " << s.rules << "\n"
        << "# TYPE ipban_hub_expiry_queue gauge\n"
        << "ipban_hub_expiry_queue " << s.expiry << "\n"
        << "# TYPE ipban_hub_sessions gauge\n"
        << "ipban_hub_sessions " << s.sessions << "\n"
        << "# TYPE ipban_hub_sessions_peak gauge\n"
        << "ipban_hub_sessions_peak " << s.sessionsPeak << "\n"
        << "# TYPE ipban_hub_link_total counter\n";
    for(size_t i = 0; i < 3; ++i)
        out << "ipban_hub_link_total{result=\"" << result[i] << "\"} " << s.links[i] << "\n";
    out << "# TYPE ipban_hub_link_errors_total counter\n";
    for(size_t i = 0; i < 5; ++i)
        out << "ipban_hub_link_errors_total{kind=\"" << err[i] << "\"} " << s.linkErr[i] << "\n";
    out << "# TYPE ipban_hub_risks_total counter\n";
    for(size_t si = 0; si < 6; ++si) {
        for(size_t ri = 0; ri < 3; ++ri) {
            out << "ipban_hub_risks_total{result=\"" << result[ri]
                << "\",source=\"" << src[si] << "\"} " << s.risks[si * 3 + ri] << "\n";
        }
    }
    out << "# TYPE ipban_hub_changes_total counter\n";
    for(size_t si = 0; si < 6; ++si) {
        for(size_t oi = 0; oi < 3; ++oi) {
            out << "ipban_hub_changes_total{op=\"" << op[oi]
                << "\",source=\"" << src[si] << "\"} " << s.changes[si * 3 + oi] << "\n";
        }
    }
    out << "# TYPE ipban_hub_db_enabled gauge\n"
        << "ipban_hub_db_enabled " << (s.db.enabled ? 1 : 0) << "\n"
        << "# TYPE ipban_hub_db_ok gauge\n"
        << "ipban_hub_db_ok " << (s.db.ok ? 1 : 0) << "\n"
        << "# TYPE ipban_hub_db_pending gauge\n"
        << "ipban_hub_db_pending " << s.db.pending << "\n"
        << "# TYPE ipban_hub_db_writes_total counter\n";
    for(size_t oi = 0; oi < 3; ++oi) {
        for(size_t ri = 0; ri < 3; ++ri) {
            out << "ipban_hub_db_writes_total{op=\"" << op[oi]
                << "\",result=\"" << result[ri] << "\"} "
                << s.db.writes[oi * 3 + ri] << "\n";
        }
    }
    out << "# TYPE ipban_hub_db_restore_total counter\n";
    for(size_t i = 0; i < 3; ++i)
        out << "ipban_hub_db_restore_total{result=\"" << result[i] << "\"} " << s.db.restore[i] << "\n";
    out << "# TYPE ipban_hub_db_restore_rules gauge\n"
        << "ipban_hub_db_restore_rules " << s.db.restoreRules << "\n"
        << "# TYPE ipban_hub_db_write_seconds histogram\n";
    uint64_t sum = 0;
    for(size_t i = 0; i < 10; ++i) {
        sum += s.db.time[i];
        out << "ipban_hub_db_write_seconds_bucket{le=\"" << le[i] << "\"} " << sum << "\n";
    }
    out << "ipban_hub_db_write_seconds_sum " << (double)s.db.timeUs / 1000000.0 << "\n"
        << "ipban_hub_db_write_seconds_count " << s.db.timeCount << "\n";
    return out.str();
}

HubHttp::HubHttp(Daemon* daemon, bronx::BxIoManager* iom)
    : bronx::BxTcpServer(iom, iom), m_daemon(daemon) {
    type_ = "banhub_http";
    setRecvTimeout(3000);
    TcpServerOptions opts;
    opts.maxConnections = 64;
    opts.drainTimeoutMs = 1000;
    setOptions(opts);
}

void HubHttp::onConnection(bronx::BxSocket::ptr client) {
    std::string head;
    head.reserve(1024);
    char buf[512];
    while(head.size() < 4096 && head.find("\r\n\r\n") == std::string::npos) {
        int n = client->recv(buf, std::min(sizeof(buf), 4096 - head.size()));
        if(n <= 0) return;
        head.append(buf, n);
    }
    size_t end = head.find("\r\n");
    if(end == std::string::npos) return;
    std::string line = head.substr(0, end);
    HubStat stat = m_daemon ? m_daemon->hubStat() : HubStat{};
    if(line == "GET /healthz HTTP/1.1" || line == "GET /healthz HTTP/1.0") {
        bool ok = stat.up && (!stat.db.enabled || stat.db.ok);
        reply(client, ok ? 200 : 503, ok ? "OK" : "Unavailable", ok ? "ok\n" : "db failed\n");
        return;
    }
    if(line == "GET /metrics HTTP/1.1" || line == "GET /metrics HTTP/1.0") {
        reply(client, 200, "OK", metrics(stat));
        return;
    }
    reply(client, 404, "Not Found", "not found\n");
}

} // namespace ipban
} // namespace bronx
