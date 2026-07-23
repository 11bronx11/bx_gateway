#include "admin.h"
#include "metrics.h"
#include "conn.h"
#include "http_msg.h"
#include "buf.h"
#include "log.h"
#include "router.h"
#include "ups_group.h"
#include "ronduan.h"
#include "middlewares/builtin.h"
#include "io.h"
#include <sstream>

namespace bronx {
namespace gateway {

static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

AdminServer::AdminServer(GatewayServer* gw,
                         bronx::BxIoManager* ioworker,
                         bronx::BxIoManager* acceptWorker)
    : bronx::BxTcpServer(ioworker, acceptWorker), m_gw(gw) {
    type_ = "gateway_admin";
}

static void sendPlain(bronx::BxSocket::ptr& sock, int status,
                       const std::string& body,
                       const std::string& ct = "text/plain") {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << status << " \r\n"
       << "Content-Type: " << ct << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n" << body;
    auto s = ss.str();
    SendAll(sock, s.data(), s.size());
}

static std::string json_esc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for(char c : s) {
        switch(c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string prom_esc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for(unsigned char c : s) {
        switch(c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            default: out += c < 0x20 && c != '\t' ? ' ' : (char)c; break;
        }
    }
    return out;
}

static const char* matchTypeName(MatchType t) {
    return t == MatchType::EXACT ? "exact" : "prefix";
}

static const char* cbStateName(CircuitBreaker::State s) {
    switch(s) {
        case CircuitBreaker::State::CLOSED: return "closed";
        case CircuitBreaker::State::OPEN: return "open";
        case CircuitBreaker::State::HALF_OPEN: return "half_open";
    }
    return "unknown";
}

static void write_str_arr(std::ostringstream& j, const std::vector<std::string>& values) {
    j << "[";
    for(size_t i = 0; i < values.size(); ++i) {
        if(i) j << ",";
        j << "\"" << json_esc(values[i]) << "\"";
    }
    j << "]";
}

static std::string ep_name(const Endpoint& ep) {
    return ep.host + ":" + std::to_string(ep.port);
}

static void write_up_metrics(std::ostringstream& m, const ConfigSnapshot::ptr& cfg) {
    m << "# TYPE gateway_upstream_requests_total counter\n"
      << "# TYPE gateway_upstream_inflight gauge\n"
      << "# TYPE gateway_upstream_latency_ms histogram\n"
      << "# TYPE gateway_upstream_rejected_total counter\n"
      << "# TYPE gateway_circuit_state gauge\n"
      << "# TYPE gateway_circuit_transitions_total counter\n";
    if(!cfg || !cfg->upstreams) return;
    static const char* states[] = {"closed", "open", "half_open"};
    for(const auto& kv : cfg->upstreams->all()) {
        const std::string up = prom_esc(kv.first);
        for(const auto& ep : kv.second->endpoints()) {
            if(!ep) continue;
            const std::string name = prom_esc(ep_name(*ep));
            for(size_t mi = 0; mi < UpStat::kMarks; ++mi) {
                for(size_t wi = 0; wi < UpStat::kWhys; ++wi) {
                    auto mark = (UpMark)mi;
                    auto why = (UpWhy)wi;
                    uint64_t n = ep->stat.requests(mark, why);
                    if(n == 0) continue;
                    m << "gateway_upstream_requests_total{upstream=\"" << up
                      << "\",endpoint=\"" << name << "\",result=\""
                      << upMarkName(mark) << "\",reason=\"" << upWhyName(why)
                      << "\"} " << n << "\n";
                }
            }
            m << "gateway_upstream_inflight{upstream=\"" << up
              << "\",endpoint=\"" << name << "\"} "
              << ep->activeConns.load(std::memory_order_relaxed) << "\n";
            uint64_t sum = 0;
            for(size_t i = 0; i < UpStat::kBounds.size(); ++i) {
                sum += ep->stat.latency(i);
                m << "gateway_upstream_latency_ms{upstream=\"" << up
                  << "\",endpoint=\"" << name << "\",bucket=\"";
                if(UpStat::kBounds[i] == UINT64_MAX) m << "+Inf";
                else m << UpStat::kBounds[i];
                m << "\"} " << sum << "\n";
            }
            for(size_t wi = 0; wi < UpStat::kWhys; ++wi) {
                auto why = (UpWhy)wi;
                uint64_t n = ep->stat.rejects(why);
                if(n == 0) continue;
                m << "gateway_upstream_rejected_total{upstream=\"" << up
                  << "\",endpoint=\"" << name << "\",reason=\""
                  << upWhyName(why) << "\"} " << n << "\n";
            }
            auto state = ep->cb->state();
            auto cb = ep->cb->snapshot();
            m << "gateway_circuit_state{upstream=\"" << up
              << "\",endpoint=\"" << name << "\"} " << (size_t)state << "\n";
            for(size_t from = 0; from < 3; ++from) {
                for(size_t to = 0; to < 3; ++to) {
                    m << "gateway_circuit_transitions_total{upstream=\"" << up
                      << "\",endpoint=\"" << name << "\",from=\"" << states[from]
                      << "\",to=\"" << states[to] << "\"} "
                      << cb.moves[from * 3 + to] << "\n";
                }
            }
        }
    }
}

static std::string metrics_text(const GatewayMetrics::Snapshot& s,
                                const ConfigSnapshot::ptr& cfg) {
    static const char* stage[] = {"header", "handler"};
    static const char* reqResult[] = {"ok", "rejected", "write_failed"};
    static const char* respResult[] = {"sent", "write_failed"};
    static const char* cls[] = {"1xx", "2xx", "3xx", "4xx", "5xx"};
    static const char* cut[] = {
        "", "bad_header", "body_too_large", "headers_too_large", "bad_body", "no_response"
    };
    static const char* le[] = {
        "0.001", "0.005", "0.01", "0.025", "0.05", "0.1",
        "0.25", "0.5", "1", "2.5", "5", "+Inf"
    };
    std::ostringstream m;
    m << "# TYPE gateway_requests_total counter\n";
    for(size_t si = 0; si < 2; ++si) {
        for(size_t ri = 0; ri < 3; ++ri) {
            m << "gateway_requests_total{stage=\"" << stage[si]
              << "\",result=\"" << reqResult[ri] << "\"} "
              << s.reqs[si * 3 + ri] << "\n";
        }
    }
    m << "# TYPE gateway_responses_total counter\n";
    for(size_t ci = 0; ci < 5; ++ci) {
        for(size_t ri = 0; ri < 2; ++ri) {
            m << "gateway_responses_total{class=\"" << cls[ci]
              << "\",result=\"" << respResult[ri] << "\"} "
              << s.responses[ci * 2 + ri] << "\n";
        }
    }
    m << "# TYPE gateway_request_rejects_total counter\n";
    for(size_t i = 1; i < s.rejects.size(); ++i)
        m << "gateway_request_rejects_total{reason=\"" << cut[i] << "\"} " << s.rejects[i] << "\n";
    m << "# TYPE gateway_client_write_errors_total counter\n"
      << "gateway_client_write_errors_total " << s.clientWriteErrors << "\n"
      << "# TYPE gateway_request_seconds histogram\n";
    uint64_t reqSum = 0;
    for(size_t i = 0; i < s.reqTime.size(); ++i) {
        reqSum += s.reqTime[i];
        m << "gateway_request_seconds_bucket{le=\"" << le[i] << "\"} " << reqSum << "\n";
    }
    m << "gateway_request_seconds_sum " << (double)s.reqTimeUs / 1000000.0 << "\n"
      << "gateway_request_seconds_count " << s.reqTimeCount << "\n"
      << "# TYPE gateway_upstream_total counter\n"
      << "gateway_upstream_total{result=\"ok\"} " << s.upstreamOk << "\n"
      << "gateway_upstream_total{result=\"fail\"} " << s.upstreamFail << "\n"
      << "# TYPE gateway_circuit_open_total counter\n"
      << "gateway_circuit_open_total " << s.circuitOpen << "\n"
      << "# TYPE gateway_upstream_acquire_fail_total counter\n"
      << "gateway_upstream_acquire_fail_total " << s.upstreamAcquireFail << "\n"
      << "# TYPE gateway_no_healthy_endpoint_total counter\n"
      << "gateway_no_healthy_endpoint_total " << s.noHealthyEndpoint << "\n"
      << "# TYPE gateway_rate_limited_total counter\n"
      << "gateway_rate_limited_total " << s.rateLimited << "\n"
      << "# TYPE gateway_cors_preflight_total counter\n"
      << "gateway_cors_preflight_total " << s.corsPreflight << "\n"
      << "# TYPE gateway_cors_denied_total counter\n"
      << "gateway_cors_denied_total " << s.corsDenied << "\n"
      << "# TYPE gateway_maintenance_blocked_total counter\n"
      << "gateway_maintenance_blocked_total " << s.maintBlocked << "\n"
      << "# TYPE gateway_active_connections gauge\n"
      << "gateway_active_connections " << s.activeConns << "\n";
    static const char* syncKind[] = {"snap", "delta"};
    static const char* syncResult[] = {"ok", "failed"};
    static const char* syncWhy[] = {"parse", "epoch", "version"};
    static const char* ruleKind[] = {"static", "remote", "total"};
    static const char* ipSrc[] = {"waf", "rate", "biz", "admin", "static", "emerg", "unknown"};
    static const char* riskResult[] = {"queued", "dedup", "full", "stopping", "sent", "failed"};
    static const char* denyWhy[] = {"rule", "unresolved"};
    static const char* wafRule[] = {"sqli", "xss", "travers", "scanner"};
    m << "# TYPE ipban_sync_apply_total counter\n";
    for(size_t ki = 0; ki < 2; ++ki) {
        for(size_t ri = 0; ri < 2; ++ri)
            m << "ipban_sync_apply_total{kind=\"" << syncKind[ki]
              << "\",result=\"" << syncResult[ri] << "\"} " << s.syncApply[ki * 2 + ri] << "\n";
    }
    m << "# TYPE ipban_sync_resync_total counter\n";
    for(size_t i = 0; i < 3; ++i)
        m << "ipban_sync_resync_total{reason=\"" << syncWhy[i] << "\"} " << s.syncResync[i] << "\n";
    m << "# TYPE ipban_sync_epoch_changes_total counter\n"
      << "ipban_sync_epoch_changes_total " << s.epochChanges << "\n"
      << "# TYPE ipban_rules gauge\n";
    for(size_t i = 0; i < 3; ++i)
        m << "ipban_rules{kind=\"" << ruleKind[i] << "\"} " << s.ipbanRules[i] << "\n";
    m << "# TYPE ipban_risk_total counter\n";
    for(size_t si = 0; si < 6; ++si) {
        for(size_t ri = 0; ri < 6; ++ri)
            m << "ipban_risk_total{result=\"" << riskResult[ri]
              << "\",source=\"" << ipSrc[si] << "\"} " << s.riskByResult[si * 6 + ri] << "\n";
    }
    m << "# TYPE ipban_risk_queue gauge\n"
      << "ipban_risk_queue " << s.riskQueue << "\n"
      << "# TYPE ipban_risk_retries_total counter\n"
      << "ipban_risk_retries_total " << s.riskRetries << "\n"
      << "# TYPE ipban_denied_total counter\n";
    for(size_t si = 0; si < 7; ++si) {
        for(size_t ri = 0; ri < 2; ++ri)
            m << "ipban_denied_total{reason=\"" << denyWhy[ri]
              << "\",source=\"" << ipSrc[si] << "\"} " << s.denied[si * 2 + ri] << "\n";
    }
    m << "# TYPE waf_denied_total counter\n";
    for(size_t wi = 0; wi < 4; ++wi) {
        m << "waf_denied_total{reported=\"true\",rule=\"" << wafRule[wi]
          << "\"} " << s.wafByRule[wi * 2] << "\n"
          << "waf_denied_total{reported=\"false\",rule=\"" << wafRule[wi]
          << "\"} " << s.wafByRule[wi * 2 + 1] << "\n";
    }
    m << "# TYPE ipban_snapshot_version gauge\n"
      << "ipban_snapshot_version " << s.ipbanVersion << "\n"
      << "# TYPE ipban_sync_connected gauge\n"
      << "ipban_sync_connected " << s.ipbanSynced << "\n"
      << "# TYPE ipban_risk_sent_total counter\n"
      << "ipban_risk_sent_total " << s.riskSent << "\n"
      << "# TYPE ipban_risk_dropped_total counter\n"
      << "ipban_risk_dropped_total " << s.riskDropped << "\n"
      << "# TYPE ipban_link_up gauge\n"
      << "ipban_link_up " << s.ipbanLinkUp << "\n"
      << "# TYPE ipban_link_state gauge\n"
      << "ipban_link_state " << s.ipbanLinkState << "\n"
      << "# TYPE ipban_sync_lag gauge\n"
      << "ipban_sync_lag " << s.ipbanSyncLag << "\n"
      << "# TYPE ipban_rx_idle_ms gauge\n"
      << "ipban_rx_idle_ms " << s.ipbanRxIdle << "\n"
      << "# TYPE ipban_ack_idle_ms gauge\n"
      << "ipban_ack_idle_ms " << s.ipbanAckIdle << "\n"
      << "# TYPE ipban_ping_ms gauge\n"
      << "ipban_ping_ms " << s.ipbanPingMs << "\n"
      << "# TYPE risk_queue gauge\n"
      << "risk_queue " << s.riskQueue << "\n"
      << "# TYPE ipban_connects_total counter\n"
      << "ipban_connects_total " << s.ipbanConnects << "\n"
      << "# TYPE ipban_reconnects_total counter\n"
      << "ipban_reconnects_total " << s.ipbanReconnects << "\n"
      << "# TYPE ipban_rx_frames_total counter\n"
      << "ipban_rx_frames_total " << s.ipbanRxFrames << "\n"
      << "# TYPE ipban_tx_frames_total counter\n"
      << "ipban_tx_frames_total " << s.ipbanTxFrames << "\n"
      << "# TYPE ipban_frame_errors_total counter\n"
      << "ipban_frame_errors_total " << s.ipbanFrameErrors << "\n"
      << "# TYPE ipban_msg_errors_total counter\n"
      << "ipban_msg_errors_total " << s.ipbanMsgErrors << "\n"
      << "# TYPE ipban_ack_timeouts_total counter\n"
      << "ipban_ack_timeouts_total " << s.ipbanAckTimeouts << "\n"
      << "# TYPE ipban_pong_timeouts_total counter\n"
      << "ipban_pong_timeouts_total " << s.ipbanPongTimeouts << "\n"
      << "# TYPE ipban_stop_grace_total counter\n"
      << "ipban_stop_grace_total " << s.ipbanStopGrace << "\n"
      << "# TYPE ipban_stop_force_total counter\n"
      << "ipban_stop_force_total " << s.ipbanStopForce << "\n"
      << "# TYPE risk_stop_dropped_total counter\n"
      << "risk_stop_dropped_total " << s.riskStopDropped << "\n"
      << "# TYPE ipban_rate_reported_total counter\n"
      << "ipban_rate_reported_total " << s.rateReported << "\n"
      << "# TYPE gateway_auth_total counter\n";
    static const char* authScheme[] = {"jwt", "api_key", "other"};
    static const char* authResult[] = {
        "ok", "missing", "bad_token", "bad_key", "bad_sig", "expired",
        "bad_iss", "bad_aud", "bad_alg", "scope", "role", "bad_policy"
    };
    for(const auto& row : s.authByRoute) {
        std::string route = prom_esc(row.first);
        for(size_t si = 0; si < 3; ++si) {
            for(size_t ri = 0; ri < 12; ++ri) {
                m << "gateway_auth_total{scheme=\"" << authScheme[si]
                  << "\",result=\"" << authResult[ri] << "\",route=\""
                  << route << "\"} " << row.second[si * 12 + ri] << "\n";
            }
        }
    }
    m << "# TYPE gw_write_fail counter\n"
      << "gw_write_fail " << s.writeFail << "\n"
      << "# TYPE gw_finish_500 counter\n"
      << "gw_finish_500 " << s.finish500 << "\n"
      << "# TYPE gw_ws_active gauge\n"
      << "gw_ws_active " << s.wsActive << "\n"
      << "# TYPE gw_ws_open counter\n"
      << "gw_ws_open " << s.wsOpen << "\n"
      << "# TYPE gw_ws_close counter\n"
      << "gw_ws_close " << s.wsClose << "\n"
      << "# TYPE gw_ws_duration_ms gauge\n"
      << "gw_ws_duration_ms " << s.wsDurationMs << "\n"
      << "# TYPE gateway_request_latency_ms gauge\n"
      << "gateway_request_latency_ms{quantile=\"0.50\"} " << s.latP50Ms << "\n"
      << "gateway_request_latency_ms{quantile=\"0.99\"} " << s.latP99Ms << "\n"
      << "gateway_request_latency_ms{quantile=\"avg\"} " << s.latAvgMs << "\n"
      << "# TYPE gateway_route_requests_total counter\n";
    for(const auto& r : s.routes) {
        std::string route = prom_esc(r.first);
        m << "gateway_route_requests_total{route=\"" << route << "\"} " << r.second.requests << "\n"
          << "gateway_route_responses_total{route=\"" << route << "\",class=\"2xx\"} " << r.second.resp2xx << "\n"
          << "gateway_route_responses_total{route=\"" << route << "\",class=\"4xx\"} " << r.second.resp4xx << "\n"
          << "gateway_route_responses_total{route=\"" << route << "\",class=\"5xx\"} " << r.second.resp5xx << "\n";
    }
    write_up_metrics(m, cfg);
    return m.str();
}

static void write_up_json(std::ostringstream& j, const ConfigSnapshot::ptr& cfg) {
    j << "[";
    bool firstEp = true;
    if(cfg && cfg->upstreams) {
        for(const auto& kv : cfg->upstreams->all()) {
            for(const auto& ep : kv.second->endpoints()) {
                if(!ep) continue;
                if(!firstEp) j << ",";
                firstEp = false;
                auto cb = ep->cb->snapshot();
                j << "{\"upstream\":\"" << json_esc(kv.first)
                  << "\",\"endpoint\":\"" << json_esc(ep_name(*ep))
                  << "\",\"inflight\":" << ep->activeConns.load(std::memory_order_relaxed)
                  << ",\"requests\":[";
                bool first = true;
                for(size_t mi = 0; mi < UpStat::kMarks; ++mi) {
                    for(size_t wi = 0; wi < UpStat::kWhys; ++wi) {
                        auto mark = (UpMark)mi;
                        auto why = (UpWhy)wi;
                        uint64_t n = ep->stat.requests(mark, why);
                        if(n == 0) continue;
                        if(!first) j << ",";
                        first = false;
                        j << "{\"result\":\"" << upMarkName(mark)
                          << "\",\"reason\":\"" << upWhyName(why)
                          << "\",\"count\":" << n << "}";
                    }
                }
                j << "],\"rejected\":[";
                first = true;
                for(size_t wi = 0; wi < UpStat::kWhys; ++wi) {
                    auto why = (UpWhy)wi;
                    uint64_t n = ep->stat.rejects(why);
                    if(n == 0) continue;
                    if(!first) j << ",";
                    first = false;
                    j << "{\"reason\":\"" << upWhyName(why)
                      << "\",\"count\":" << n << "}";
                }
                j << "],\"latency\":{\"count\":" << ep->stat.latencyCount()
                  << ",\"sum_ms\":" << ep->stat.latencySum() << "}"
                  << ",\"circuit\":{\"state\":\"" << cbStateName(ep->cb->state())
                  << "\",\"requests\":" << cb.requests
                  << ",\"failures\":" << cb.failures
                  << ",\"slows\":" << cb.slows
                  << ",\"failure_rate\":" << cb.failureRate
                  << ",\"slow_rate\":" << cb.slowRate
                  << ",\"open_ms\":" << cb.openMs << "}}";
            }
        }
    }
    j << "]";
}

void AdminServer::onConnection(bronx::BxSocket::ptr client) {
    InBuf buf(4096);
    size_t headLen = 0;
    for(int i = 0; i < 20 && headLen == 0; ++i) {
        if(buf.fill(client.get()) <= 0) return;
        headLen = buf.findHeaderEnd();
    }
    if(headLen == 0) return;

    HttpReqParser parser;
    std::string head(buf.peek(), headLen);
    parser.execute(head.c_str(), headLen);
    if(parser.hasError() || !parser.isFinished()) return;

    auto req = parser.getData();
    std::string path = req->getPath();
    std::string method = HttpMethodToString(req->getMethod());

    if(path == "/healthz") {
        sendPlain(client, 200, "OK\n");
        return;
    }
    if(path == "/stats" && method == "GET") {
        auto snap = GatewayMetrics::instance().snapshot();
        auto cfg = m_gw ? m_gw->getConfig() : nullptr;
        std::ostringstream j;
        j << "{"
          << "\"requests\":"     << snap.requests
          << ",\"1xx\":"         << snap.resp1xx
          << ",\"2xx\":"         << snap.resp2xx
          << ",\"3xx\":"         << snap.resp3xx
          << ",\"4xx\":"         << snap.resp4xx
          << ",\"5xx\":"         << snap.resp5xx
          << ",\"upstream_ok\":" << snap.upstreamOk
          << ",\"upstream_fail\":" << snap.upstreamFail
          << ",\"circuit_open\":" << snap.circuitOpen
          << ",\"upstream_acquire_fail\":" << snap.upstreamAcquireFail
          << ",\"no_healthy_endpoint\":" << snap.noHealthyEndpoint
          << ",\"rate_limited\":" << snap.rateLimited
          << ",\"cors_preflight\":" << snap.corsPreflight
          << ",\"cors_denied\":" << snap.corsDenied
          << ",\"maint_blocked\":" << snap.maintBlocked
          << ",\"active_conns\":" << snap.activeConns
          << ",\"client_write_errors\":" << snap.clientWriteErrors
          << ",\"request_seconds_count\":" << snap.reqTimeCount
          << ",\"ipban_denied\":" << snap.ipbanDenied
          << ",\"ipban_version\":" << snap.ipbanVersion
          << ",\"ipban_synced\":" << snap.ipbanSynced
          << ",\"risk_sent\":" << snap.riskSent
          << ",\"risk_dropped\":" << snap.riskDropped
          << ",\"ipban_link_up\":" << snap.ipbanLinkUp
          << ",\"ipban_link_state\":" << snap.ipbanLinkState
          << ",\"ipban_link_state_map\":{\"0\":\"down\""
          << ",\"1\":\"dial\",\"2\":\"hello\",\"3\":\"sync\""
          << ",\"4\":\"open\",\"5\":\"backoff\""
          << ",\"6\":\"stopping\",\"7\":\"stopped\"}"
          << ",\"ipban_sync_lag\":" << snap.ipbanSyncLag
          << ",\"ipban_rx_idle_ms\":" << snap.ipbanRxIdle
          << ",\"ipban_ack_idle_ms\":" << snap.ipbanAckIdle
          << ",\"ipban_ping_ms\":" << snap.ipbanPingMs
          << ",\"risk_queue\":" << snap.riskQueue
          << ",\"ipban_connects\":" << snap.ipbanConnects
          << ",\"ipban_reconnects\":" << snap.ipbanReconnects
          << ",\"ipban_rx_frames\":" << snap.ipbanRxFrames
          << ",\"ipban_tx_frames\":" << snap.ipbanTxFrames
          << ",\"ipban_frame_errors\":" << snap.ipbanFrameErrors
          << ",\"ipban_msg_errors\":" << snap.ipbanMsgErrors
          << ",\"ipban_ack_timeouts\":" << snap.ipbanAckTimeouts
          << ",\"ipban_pong_timeouts\":" << snap.ipbanPongTimeouts
          << ",\"ipban_stop_grace\":" << snap.ipbanStopGrace
          << ",\"ipban_stop_force\":" << snap.ipbanStopForce
          << ",\"risk_stop_dropped\":" << snap.riskStopDropped
          << ",\"waf_denied\":" << snap.wafDenied
          << ",\"rate_reported\":" << snap.rateReported
          << ",\"write_fail\":" << snap.writeFail
          << ",\"finish_500\":" << snap.finish500
          << ",\"ws_active\":" << snap.wsActive
          << ",\"ws_open\":" << snap.wsOpen
          << ",\"ws_close\":" << snap.wsClose
          << ",\"ws_duration_ms\":" << snap.wsDurationMs
          << ",\"auth\":{\"ok\":" << snap.authOk
          << ",\"no_token\":" << snap.authNoToken
          << ",\"bad_sig\":" << snap.authBadSig
          << ",\"expired\":" << snap.authExpired
          << ",\"forbidden\":" << snap.authForbidden << "}"
          << ",\"latency\":{\"count\":" << snap.latCount
          << ",\"avg_ms\":" << snap.latAvgMs
          << ",\"p50_ms\":" << snap.latP50Ms
          << ",\"p99_ms\":" << snap.latP99Ms << "}"
          << ",\"routes\":{";
        bool first = true;
        for(const auto& r : snap.routes) {
            if(!first) j << ",";
            first = false;
            j << "\"" << json_esc(r.first) << "\":{"
              << "\"requests\":" << r.second.requests
              << ",\"2xx\":" << r.second.resp2xx
              << ",\"4xx\":" << r.second.resp4xx
              << ",\"5xx\":" << r.second.resp5xx << "}";
        }
        j << "},\"upstreams\":";
        write_up_json(j, cfg);
        // route cache 命中率
        uint64_t cHits = 0, cMisses = 0;
        if(cfg && cfg->router) cfg->router->cacheStats(cHits, cMisses);
        uint64_t cTotal = cHits + cMisses;
        double cRatio = cTotal > 0 ? (double)cHits / (double)cTotal : 0.0;
        j << ",\"route_cache\":{\"hits\":" << cHits
          << ",\"misses\":" << cMisses
          << ",\"ratio\":" << cRatio << "}";
        j << "}";
        sendPlain(client, 200, j.str(), "application/json");
        return;
    }
    if(path == "/metrics" && method == "GET") {
        auto cfg = m_gw ? m_gw->getConfig() : nullptr;
        sendPlain(client, 200, metrics_text(GatewayMetrics::instance().snapshot(), cfg),
                  "text/plain; version=0.0.4");
        return;
    }
    if(path == "/routes" && method == "GET") {
        auto cfg = m_gw->getConfig();
        std::ostringstream j;
        j << "{\"routes\":[";
        if(cfg && cfg->router) {
            auto routes = cfg->router->listRoutes();
            for(size_t i = 0; i < routes.size(); ++i) {
                const auto& r = routes[i];
                if(i) j << ",";
                j << "{"
                  << "\"name\":\"" << json_esc(r.name) << "\""
                  << ",\"match_type\":\"" << matchTypeName(r.matchType) << "\""
                  << ",\"path\":\"" << json_esc(r.pathPattern) << "\""
                  << ",\"methods\":";
                write_str_arr(j, r.methods);
                j << ",\"host\":\"" << json_esc(r.host) << "\""
                  << ",\"upstream\":\"" << json_esc(r.upstream) << "\""
                  << ",\"strip_prefix\":" << (r.stripPrefix ? "true" : "false")
                  << ",\"rewrite_prefix\":\"" << json_esc(r.rewritePrefix) << "\""
                  << ",\"priority\":" << r.priority
                  << ",\"auth\":\"" << json_esc(r.authPolicy) << "\""
                  << ",\"rate_limit\":{\"enabled\":" << (r.rateLimitEnabled ? "true" : "false")
                  << ",\"capacity\":" << r.rateLimitCapacity
                  << ",\"refill_per_sec\":" << r.rateLimitRefillPerSec
                  << ",\"key\":\"" << json_esc(r.rateLimitKey) << "\"}"
                  << ",\"request_headers\":{\"set_count\":" << r.reqHeaderSet.size()
                  << ",\"remove\":";
                write_str_arr(j, r.reqHeaderRemove);
                j << "}}";
            }
        }
        j << "],\"upstreams\":[";
        if(cfg && cfg->upstreams) {
            bool firstGroup = true;
            for(const auto& kv : cfg->upstreams->all()) {
                auto g = kv.second;
                if(!firstGroup) j << ",";
                firstGroup = false;
                const auto& hc = g->healthCheck();
                j << "{\"name\":\"" << json_esc(g->name()) << "\""
                  << ",\"lb\":\"" << json_esc(g->lbName()) << "\""
                  << ",\"timeout\":{\"total_ms\":" << g->totalMs()
                  << ",\"connect_ms\":" << g->connectMs()
                  << ",\"read_ms\":" << g->readMs() << "}"
                  << ",\"health_check\":{\"enabled\":" << (hc.enabled ? "true" : "false")
                  << ",\"path\":\"" << json_esc(hc.path) << "\""
                  << ",\"interval_ms\":" << hc.intervalMs
                  << ",\"timeout_ms\":" << hc.timeoutMs
                  << ",\"healthy_threshold\":" << hc.healthyThreshold
                  << ",\"unhealthy_threshold\":" << hc.unhealthyThreshold << "}"
                  << ",\"endpoints\":[";
                auto eps = g->endpoints();
                for(size_t i = 0; i < eps.size(); ++i) {
                    auto& ep = eps[i];
                    if(i) j << ",";
                    const auto& cbCfg = ep->cb->config();
                    auto cbSnap = ep->cb->snapshot();
                    const auto& poolCfg = ep->pool->config();
                    j << "{\"host\":\"" << json_esc(ep->host) << "\""
                      << ",\"port\":" << ep->port
                      << ",\"weight\":" << ep->weight
                      << ",\"max_inflight\":" << ep->maxInflight
                      << ",\"healthy\":" << (ep->healthy.load() ? "true" : "false")
                      << ",\"active_conns\":" << ep->activeConns.load()
                      << ",\"pool\":{\"idle\":" << ep->pool->idleCount()
                      << ",\"max_idle\":" << poolCfg.maxIdle
                      << ",\"idle_timeout_ms\":" << poolCfg.idleTimeoutMs << "}"
                      << ",\"circuit\":\"" << cbStateName(ep->cb->state()) << "\""
                      << ",\"circuit_breaker\":{\"failure_threshold\":" << cbCfg.failureThreshold
                      << ",\"window_ms\":" << cbCfg.windowMs
                      << ",\"buckets\":" << cbCfg.buckets
                      << ",\"min_requests\":" << cbCfg.minRequests
                      << ",\"failure_rate\":" << cbCfg.failureRate
                      << ",\"slow_ms\":" << cbCfg.slowMs
                      << ",\"slow_rate\":" << cbCfg.slowRate
                      << ",\"open_timeout_ms\":" << cbCfg.openTimeoutMs
                      << ",\"max_open_timeout_ms\":" << cbCfg.maxOpenTimeoutMs
                      << ",\"half_open_max_requests\":" << cbCfg.halfOpenMaxRequests
                      << ",\"half_open_successes\":" << cbCfg.halfOpenSuccesses
                      << ",\"window_requests\":" << cbSnap.requests
                      << ",\"window_failures\":" << cbSnap.failures
                      << ",\"window_slows\":" << cbSnap.slows << "}"
                      << ",\"health_checks\":" << ep->healthChecks.load()
                      << ",\"health_successes\":" << ep->healthSuccesses.load()
                      << ",\"health_failures\":" << ep->healthFailures.load()
                      << ",\"health_check_fails\":" << ep->healthCheckFails.load()
                      << ",\"last_health_status\":" << ep->lastHealthStatus.load()
                      << "}";
                }
                j << "]}";
            }
        }
        j << "]}";
        sendPlain(client, 200, j.str(), "application/json");
        return;
    }
    if(path == "/reload" && method == "POST") {
        bool ok = m_gw->reload("");
        sendPlain(client, ok ? 200 : 500, ok ? "reloaded\n" : "reload failed\n");
        return;
    }
    if(path == "/maintenance" && method == "GET") {
        bool on = MaintGate::instance().on();
        sendPlain(client, 200, std::string("{\"maintenance\":") + (on ? "true" : "false") + "}\n",
                  "application/json");
        return;
    }
    if(path == "/maintenance/on" && method == "POST") {
        MaintGate::instance().set(true);
        BRONX_LOG_WARN(g_logger) << "maintenance mode ON";
        sendPlain(client, 200, "maintenance on\n");
        return;
    }
    if(path == "/maintenance/off" && method == "POST") {
        MaintGate::instance().set(false);
        BRONX_LOG_WARN(g_logger) << "maintenance mode OFF";
        sendPlain(client, 200, "maintenance off\n");
        return;
    }
    sendPlain(client, 404, "Not Found\n");
}

} // namespace gateway
} // namespace bronx
