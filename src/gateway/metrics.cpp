#include "metrics.h"
#include <cmath>

namespace bronx {
namespace gateway {

void GatewayMetrics::finishReq(ReqStage stage, ReqResult result, int status,
                               ReqCut cut, uint64_t us) {
    size_t si = static_cast<size_t>(stage);
    size_t ri = static_cast<size_t>(result);
    if(si >= reqs.size() / 3 || ri >= 3) return;
    ++reqs[si * 3 + ri];
    if(status >= 100 && status <= 599) {
        size_t cls = static_cast<size_t>(status / 100 - 1);
        size_t sent = result == ReqResult::WRITE_FAIL ? 1 : 0;
        ++responses[cls * 2 + sent];
    }
    size_t ci = static_cast<size_t>(cut);
    if(cut != ReqCut::NONE && ci < rejects.size()) ++rejects[ci];
    if(result == ReqResult::WRITE_FAIL) ++clientWriteErrors;
    for(size_t i = 0; i < kBucketBounds.size(); ++i) {
        if(kBucketBounds[i] == UINT32_MAX
           || us <= static_cast<uint64_t>(kBucketBounds[i]) * 1000) {
            ++reqTime[i];
            break;
        }
    }
    reqTimeUs += us;
}

void GatewayMetrics::recordUp(const UpRet& ret) {
    if(ret.mark == UpMark::OK) ++upstreamOk;
    else if(ret.mark == UpMark::FAIL) ++upstreamFail;
}

// 从直方图桶估算分位数：找到累计计数跨过 p*total 的桶，返回其上界。
// 近似（桶粒度），但对运维足够；避免存全部样本。
uint32_t GatewayMetrics::percentile(double p) const {
    uint64_t total = latCount.load();
    if(total == 0) return 0;
    uint64_t target = (uint64_t)std::ceil(p * total);
    if(target == 0) target = 1;
    uint64_t cum = 0;
    for(size_t i = 0; i < kBucketBounds.size(); ++i) {
        cum += latBuckets[i].load();
        if(cum >= target) {
            uint32_t b = kBucketBounds[i];
            return b == UINT32_MAX ? kBucketBounds[kBucketBounds.size()-2] : b;
        }
    }
    return kBucketBounds[kBucketBounds.size()-2];
}

GatewayMetrics::Snapshot GatewayMetrics::snapshot() const {
    Snapshot s;
    s.requests = 0;
    for(size_t i = 0; i < s.reqs.size(); ++i) {
        s.reqs[i] = reqs[i].load();
        s.requests += s.reqs[i];
    }
    for(size_t i = 0; i < s.responses.size(); ++i)
        s.responses[i] = responses[i].load();
    s.resp1xx = s.responses[0] + s.responses[1];
    s.resp2xx = s.responses[2] + s.responses[3];
    s.resp3xx = s.responses[4] + s.responses[5];
    s.resp4xx = s.responses[6] + s.responses[7];
    s.resp5xx = s.responses[8] + s.responses[9];
    for(size_t i = 0; i < s.rejects.size(); ++i)
        s.rejects[i] = rejects[i].load();
    s.clientWriteErrors = clientWriteErrors.load();
    s.reqTimeCount = 0;
    for(size_t i = 0; i < s.reqTime.size(); ++i) {
        s.reqTime[i] = reqTime[i].load();
        s.reqTimeCount += s.reqTime[i];
    }
    s.reqTimeUs = reqTimeUs.load();
    s.upstreamOk   = upstreamOk.load();
    s.upstreamFail = upstreamFail.load();
    s.circuitOpen  = circuitOpen.load();
    s.upstreamAcquireFail = upstreamAcquireFail.load();
    s.noHealthyEndpoint = noHealthyEndpoint.load();
    s.rateLimited  = rateLimited.load();
    s.corsPreflight = corsPreflight.load();
    s.corsDenied   = corsDenied.load();
    s.maintBlocked = maintBlocked.load();
    s.activeConns  = activeConns.load();
    s.ipbanDenied  = ipbanDenied.load();
    s.ipbanVersion = ipbanVersion.load();
    s.ipbanSynced  = ipbanSynced.load();
    s.riskSent     = riskSent.load();
    s.riskDropped  = riskDropped.load();
    s.ipbanLinkUp = ipbanLinkUp.load();
    s.ipbanLinkState = ipbanLinkState.load();
    s.ipbanSyncLag = ipbanSyncLag.load();
    s.ipbanRxIdle = ipbanRxIdle.load();
    s.ipbanAckIdle = ipbanAckIdle.load();
    s.ipbanPingMs = ipbanPingMs.load();
    s.riskQueue = riskQueue.load();
    s.ipbanConnects = ipbanConnects.load();
    s.ipbanReconnects = ipbanReconnects.load();
    s.ipbanRxFrames = ipbanRxFrames.load();
    s.ipbanTxFrames = ipbanTxFrames.load();
    s.ipbanFrameErrors = ipbanFrameErrors.load();
    s.ipbanMsgErrors = ipbanMsgErrors.load();
    s.ipbanAckTimeouts = ipbanAckTimeouts.load();
    s.ipbanPongTimeouts = ipbanPongTimeouts.load();
    s.ipbanStopGrace = ipbanStopGrace.load();
    s.ipbanStopForce = ipbanStopForce.load();
    s.riskStopDropped = riskStopDropped.load();
    for(size_t i = 0; i < s.syncApply.size(); ++i) s.syncApply[i] = syncApply[i].load();
    for(size_t i = 0; i < s.syncResync.size(); ++i) s.syncResync[i] = syncResync[i].load();
    s.epochChanges = epochChanges.load();
    for(size_t i = 0; i < s.ipbanRules.size(); ++i) s.ipbanRules[i] = ipbanRules[i].load();
    for(size_t i = 0; i < s.riskByResult.size(); ++i) s.riskByResult[i] = riskByResult[i].load();
    s.riskRetries = riskRetries.load();
    for(size_t i = 0; i < s.denied.size(); ++i) s.denied[i] = denied[i].load();
    for(size_t i = 0; i < s.wafByRule.size(); ++i) s.wafByRule[i] = wafByRule[i].load();
    {
        std::lock_guard<std::mutex> lk(authMtx);
        s.authByRoute.reserve(authByRoute.size());
        for(const auto& row : authByRoute) s.authByRoute.push_back(row);
    }
    s.wafDenied    = wafDenied.load();
    s.rateReported = rateReported.load();
    s.authOk        = authOk.load();
    s.authNoToken   = authNoToken.load();
    s.authBadSig    = authBadSig.load();
    s.authExpired   = authExpired.load();
    s.authForbidden = authForbidden.load();
    s.writeFail = writeFail.load();
    s.finish500 = finish500.load();
    s.wsOpen = wsOpenCount.load();
    s.wsClose = wsCloseCount.load();
    s.wsActive = wsActive.load();
    s.wsDurationMs = s.wsClose ? (double)wsDurationSum.load() / s.wsClose : 0.0;

    uint64_t cnt = latCount.load();
    s.latCount = cnt;
    s.latAvgMs = cnt ? (double)latSum.load() / cnt : 0.0;
    s.latP50Ms = percentile(0.50);
    s.latP99Ms = percentile(0.99);

    {
        std::lock_guard<std::mutex> lk(routeMtx);
        s.routes.reserve(routeMap.size());
        for(const auto& kv : routeMap) {
            s.routes.push_back({kv.first,
                {kv.second.requests, kv.second.resp2xx, kv.second.resp4xx, kv.second.resp5xx}});
        }
    }
    return s;
}

} // namespace gateway
} // namespace bronx
