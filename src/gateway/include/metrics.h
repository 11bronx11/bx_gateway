#pragma once

// 全局指标单例，给 /stats 用。计数器都是原子的，直接加。
// 延迟走固定桶的直方图，snapshot 时算 P50 P99，per-route 那份用 mutex 护着按路由名聚。

#include <atomic>
#include <cstdint>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "up_ret.h"

namespace bronx {
namespace gateway {

enum class ReqStage : uint8_t { HEAD, HANDLE };
enum class ReqResult : uint8_t { OK, REJECT, WRITE_FAIL };
enum class ReqCut : uint8_t {
    NONE, BAD_HEADER, BODY_TOO_LARGE, HEADER_TOO_LARGE, BAD_BODY, NO_RESPONSE, COUNT
};

class GatewayMetrics {
public:
    static GatewayMetrics& instance() {
        static GatewayMetrics s;
        return s;
    }

    GatewayMetrics() { for(auto& b : latBuckets) b.store(0); }

    void incrRequests()        { ++reqs[3]; }
    void incrResponses2xx()    { ++responses[2]; }
    void incrResponses4xx()    { ++responses[6]; }
    void incrResponses5xx()    { ++responses[8]; }
    void incrUpstreamOk()      { ++upstreamOk; }
    void incrUpstreamFail()    { ++upstreamFail; }
    void recordUp(const UpRet& ret);
    void incrCircuitOpen()     { ++circuitOpen; }
    void incr_acq_fail() { ++upstreamAcquireFail; }
    void incr_no_healthy()   { ++noHealthyEndpoint; }
    void incrRateLimited()     { ++rateLimited; }
    void incr_cors_preflight() { ++corsPreflight; }   // 走预检短路的次数
    void incr_cors_denied()    { ++corsDenied; }      // 带了 origin 但不在白名单
    void incr_maint_blocked()  { ++maintBlocked; }    // 维护模式挡掉的请求
    void incrConnections()     { ++activeConns; }
    void decrConnections()     { decrGauge(activeConns); }
    // ipban 名单
    void incr_ipban_denied()   { ++ipbanDenied; }     // 名单挡掉的请求
    uint64_t claim_ipban() { return ++ipbanOwner; }
    bool own_ipban(uint64_t id) const { return id && ipbanOwner.load() == id; }
    void set_ipban_version(uint64_t v, uint64_t id = 0) { if(!id || own_ipban(id)) ipbanVersion.store(v); }
    void set_ipban_synced(bool v, uint64_t id = 0) { if(!id || own_ipban(id)) ipbanSynced.store(v ? 1 : 0); }
    void incr_risk_sent()      { ++riskSent; }        // 上报出去的举报
    void incr_risk_dropped()   { ++riskDropped; }     // 队列满丢弃的举报
    void set_ipban_link_up(bool v, uint64_t id = 0) { if(!id || own_ipban(id)) ipbanLinkUp.store(v ? 1 : 0); }
    void set_ipban_link_state(uint64_t v, uint64_t id = 0) { if(!id || own_ipban(id)) ipbanLinkState.store(v); }
    void set_ipban_sync_lag(uint64_t v, uint64_t id = 0) { if(!id || own_ipban(id)) ipbanSyncLag.store(v); }
    void set_ipban_rx_idle(uint64_t v, uint64_t id = 0) { if(!id || own_ipban(id)) ipbanRxIdle.store(v); }
    void set_ipban_ack_idle(uint64_t v, uint64_t id = 0) { if(!id || own_ipban(id)) ipbanAckIdle.store(v); }
    void set_ipban_ping_ms(uint64_t v, uint64_t id = 0) { if(!id || own_ipban(id)) ipbanPingMs.store(v); }
    void set_risk_queue(uint64_t v) { riskQueue.store(v); }
    void incr_ipban_connect() { ++ipbanConnects; }
    void incr_ipban_reconnect() { ++ipbanReconnects; }
    void incr_ipban_rx() { ++ipbanRxFrames; }
    void incr_ipban_tx() { ++ipbanTxFrames; }
    void incr_ipban_frame_err() { ++ipbanFrameErrors; }
    void incr_ipban_msg_err() { ++ipbanMsgErrors; }
    void incr_ipban_ack_timeout() { ++ipbanAckTimeouts; }
    void incr_ipban_pong_timeout() { ++ipbanPongTimeouts; }
    void incr_ipban_stop_grace() { ++ipbanStopGrace; }
    void incr_ipban_stop_force() { ++ipbanStopForce; }
    void incr_risk_stop_drop(uint64_t n = 1) { riskStopDropped += n; }
    void incr_waf_denied()     { ++wafDenied; }       // waf 挡掉的请求
    void incr_rate_reported()  { ++rateReported; }    // 限流攒够阈值报出去的次数
    // 鉴权分类, 认证过 / 没 token / 签名等错 / 过期 / 授权不够
    void incr_auth_ok()        { ++authOk; }
    void incr_auth_no_token()  { ++authNoToken; }
    void incr_auth_bad_sig()   { ++authBadSig; }
    void incr_auth_expired()   { ++authExpired; }
    void incr_auth_forbidden() { ++authForbidden; }
    void incrWriteFail()       { ++writeFail; }
    void incrFinish500()       { ++finish500; }
    void wsOpen()              { ++wsOpenCount; ++wsActive; }
    void wsClose()             { ++wsCloseCount; decrGauge(wsActive); }
    void recordWsDuration(uint64_t ms) { wsDurationSum += ms; }
    void note_sync_apply(bool delta, bool ok) { ++syncApply[(delta ? 1 : 0) * 2 + (ok ? 0 : 1)]; }
    void note_sync_resync(size_t why) { if(why < syncResync.size()) ++syncResync[why]; }
    void incr_epoch_change() { ++epochChanges; }
    void set_ipban_rules(uint64_t local, uint64_t remote) {
        ipbanRules[0].store(local); ipbanRules[1].store(remote); ipbanRules[2].store(local + remote);
    }
    void note_risk(size_t src, size_t result) {
        if(src < 6 && result < 6) ++riskByResult[src * 6 + result];
    }
    void incr_risk_retry() { ++riskRetries; }
    void note_deny(size_t src, bool unresolved) {
        if(src < 7) ++denied[src * 2 + (unresolved ? 1 : 0)];
    }
    void note_waf(size_t rule, bool reported) {
        if(rule < 4) ++wafByRule[rule * 2 + (reported ? 0 : 1)];
    }
    void note_auth(size_t scheme, size_t result, const std::string& route) {
        if(scheme >= 3 || result >= 12) return;
        std::lock_guard<std::mutex> lk(authMtx);
        ++authByRoute[route.empty() ? "none" : route][scheme * 12 + result];
    }
    void finishReq(ReqStage stage, ReqResult result, int status,
                   ReqCut cut, uint64_t us);

    // 延迟桶上界(ms)，最后一桶 = +inf。与 BUCKETS 一一对应。
    static constexpr std::array<uint32_t, 12> kBucketBounds{
        1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, UINT32_MAX};

    // 记录一次请求延迟(ms)：落入对应桶 + 累加总和。
    void recordLatency(uint64_t ms) {
        for(size_t i = 0; i < kBucketBounds.size(); ++i) {
            if(ms <= kBucketBounds[i]) { ++latBuckets[i]; break; }
        }
        latSum += ms;
        ++latCount;
    }

    // per-route 计数：按路由名聚合 requests + 状态分类。
    void recordRoute(const std::string& route, int status) {
        if(route.empty()) return;
        std::lock_guard<std::mutex> lk(routeMtx);
        auto& m = routeMap[route];
        ++m.requests;
        if(status >= 200 && status < 300)      ++m.resp2xx;
        else if(status >= 400 && status < 500) ++m.resp4xx;
        else if(status >= 500)                 ++m.resp5xx;
    }

    struct RouteSnap { uint64_t requests, resp2xx, resp4xx, resp5xx; };
    struct Snapshot {
        uint64_t requests, resp2xx, resp4xx, resp5xx;
        uint64_t resp1xx, resp3xx;
        std::array<uint64_t, 6> reqs;
        std::array<uint64_t, 10> responses;
        std::array<uint64_t, 6> rejects;
        uint64_t clientWriteErrors;
        std::array<uint64_t, 12> reqTime;
        uint64_t reqTimeUs, reqTimeCount;
        uint64_t upstreamOk, upstreamFail;
        uint64_t circuitOpen, upstreamAcquireFail, noHealthyEndpoint, rateLimited;
        uint64_t corsPreflight, corsDenied, maintBlocked;
        int64_t  activeConns;
        uint64_t ipbanDenied, ipbanVersion, ipbanSynced, riskSent, riskDropped;
        uint64_t ipbanLinkUp, ipbanLinkState, ipbanSyncLag;
        uint64_t ipbanRxIdle, ipbanAckIdle, ipbanPingMs, riskQueue;
        uint64_t ipbanConnects, ipbanReconnects, ipbanRxFrames, ipbanTxFrames;
        uint64_t ipbanFrameErrors, ipbanMsgErrors;
        uint64_t ipbanAckTimeouts, ipbanPongTimeouts;
        uint64_t ipbanStopGrace, ipbanStopForce, riskStopDropped;
        std::array<uint64_t, 4> syncApply;
        std::array<uint64_t, 3> syncResync;
        uint64_t epochChanges;
        std::array<uint64_t, 3> ipbanRules;
        std::array<uint64_t, 36> riskByResult;
        uint64_t riskRetries;
        std::array<uint64_t, 14> denied;
        std::array<uint64_t, 8> wafByRule;
        std::vector<std::pair<std::string, std::array<uint64_t, 36>>> authByRoute;
        uint64_t wafDenied, rateReported;
        uint64_t authOk, authNoToken, authBadSig, authExpired, authForbidden;
        uint64_t writeFail, finish500, wsOpen, wsClose;
        int64_t wsActive;
        double wsDurationMs;
        // 延迟
        uint64_t latCount;
        double   latAvgMs, latP50Ms, latP99Ms;
        // per-route
        std::vector<std::pair<std::string, RouteSnap>> routes;
    };

    Snapshot snapshot() const;

private:
    static void decrGauge(std::atomic<int64_t>& gauge) {
        int64_t value = gauge.load();
        while(value > 0 && !gauge.compare_exchange_weak(value, value - 1)) {}
    }

    uint32_t percentile(double p) const; // p in [0,1]

    std::array<std::atomic<uint64_t>, 6> reqs{};
    std::array<std::atomic<uint64_t>, 10> responses{};
    std::array<std::atomic<uint64_t>, 6> rejects{};
    std::atomic<uint64_t> clientWriteErrors{0};
    std::array<std::atomic<uint64_t>, 12> reqTime{};
    std::atomic<uint64_t> reqTimeUs{0};
    std::atomic<uint64_t> upstreamOk{0}, upstreamFail{0};
    std::atomic<uint64_t> circuitOpen{0}, upstreamAcquireFail{0}, noHealthyEndpoint{0}, rateLimited{0};
    std::atomic<uint64_t> corsPreflight{0}, corsDenied{0}, maintBlocked{0};
    std::atomic<int64_t>  activeConns{0};
    std::atomic<uint64_t> ipbanDenied{0}, ipbanVersion{0}, ipbanSynced{0};
    std::atomic<uint64_t> ipbanOwner{0};
    std::atomic<uint64_t> riskSent{0}, riskDropped{0};
    std::atomic<uint64_t> ipbanLinkUp{0}, ipbanLinkState{0}, ipbanSyncLag{0};
    std::atomic<uint64_t> ipbanRxIdle{0}, ipbanAckIdle{0}, ipbanPingMs{0}, riskQueue{0};
    std::atomic<uint64_t> ipbanConnects{0}, ipbanReconnects{0};
    std::atomic<uint64_t> ipbanRxFrames{0}, ipbanTxFrames{0};
    std::atomic<uint64_t> ipbanFrameErrors{0}, ipbanMsgErrors{0};
    std::atomic<uint64_t> ipbanAckTimeouts{0}, ipbanPongTimeouts{0};
    std::atomic<uint64_t> ipbanStopGrace{0}, ipbanStopForce{0}, riskStopDropped{0};
    std::array<std::atomic<uint64_t>, 4> syncApply{};
    std::array<std::atomic<uint64_t>, 3> syncResync{};
    std::atomic<uint64_t> epochChanges{0};
    std::array<std::atomic<uint64_t>, 3> ipbanRules{};
    std::array<std::atomic<uint64_t>, 36> riskByResult{};
    std::atomic<uint64_t> riskRetries{0};
    std::array<std::atomic<uint64_t>, 14> denied{};
    std::array<std::atomic<uint64_t>, 8> wafByRule{};
    std::atomic<uint64_t> wafDenied{0}, rateReported{0};
    std::atomic<uint64_t> authOk{0}, authNoToken{0}, authBadSig{0}, authExpired{0}, authForbidden{0};
    std::atomic<uint64_t> writeFail{0}, finish500{0};
    std::atomic<uint64_t> wsOpenCount{0}, wsCloseCount{0}, wsDurationSum{0};
    std::atomic<int64_t> wsActive{0};

    std::array<std::atomic<uint64_t>, 12> latBuckets;
    std::atomic<uint64_t> latSum{0}, latCount{0};

    struct RouteCounters { uint64_t requests=0, resp2xx=0, resp4xx=0, resp5xx=0; };
    mutable std::mutex routeMtx;
    std::unordered_map<std::string, RouteCounters> routeMap;
    mutable std::mutex authMtx;
    std::unordered_map<std::string, std::array<uint64_t, 36>> authByRoute;
};

} // namespace gateway
} // namespace bronx
