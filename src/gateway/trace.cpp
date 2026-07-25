#include "trace.h"
#include "util.h"
#include "up_ret.h"   // upMs()

namespace bronx {
namespace gateway {

bronx::BxLogger::ptr& TraceGate::logger() {
    static bronx::BxLogger::ptr lg = BRONX_LOG_NAME("trace");
    return lg;
}

uint64_t TraceTag::nextId() {
    static std::atomic<uint64_t> seq{0};
    return seq.fetch_add(1, std::memory_order_relaxed) + 1;
}

// 序号补零到 3 位, 肉眼扫和字典序都好用; 超过 999 行(超长 SSE)自然溢出成 4 位不影响
// 数值排序(sort -t. -k2 -n)。
static void append_seq(std::string& s, uint32_t n) {
    if(n < 100) s += (n < 10 ? "00" : "0");
    s += std::to_string(n);
}

std::string TraceTag::pfx() {
    // 请求级过滤没过(只有连接级开着, 比如读头阶段的 recv)就退回不带请求号的形式,
    // 否则会打出误导人的 #0
    if(!on) return pfxConn();
    std::string s;
    s.reserve(20);
    s += "[c";
    s += std::to_string(id);
    s += '#';
    s += std::to_string(req);
    s += '.';
    append_seq(s, ++seq);
    s += "] ";
    return s;
}

std::string TraceTag::pfxConn() {
    std::string s;
    s.reserve(16);
    s += "[c";
    s += std::to_string(id);
    s += '.';
    append_seq(s, ++seq);
    s += "] ";
    return s;
}

const char* framingName(BodyFraming f) {
    switch(f) {
        case BodyFraming::NONE:           return "none";
        case BodyFraming::CONTENT_LENGTH: return "CL";
        case BodyFraming::CHUNKED:        return "chunked";
        case BodyFraming::UNTIL_CLOSE:    return "until_close";
    }
    return "?";
}

const char* reqCutName(ReqCut c) {
    switch(c) {
        case ReqCut::NONE:             return "none";
        case ReqCut::BAD_HEADER:       return "bad_header";
        case ReqCut::BODY_TOO_LARGE:   return "body_too_large";
        case ReqCut::HEADER_TOO_LARGE: return "header_too_large";
        case ReqCut::BAD_BODY:         return "bad_body";
        case ReqCut::NO_RESPONSE:      return "no_response";
        default:                       return "?";
    }
}

namespace {
constexpr uint32_t kBlkStep = 50;      // 每 50 块打一条
constexpr uint64_t kMsStep  = 1000;    // 或静默满 1 秒打一条
constexpr uint32_t kBlkStepDeep = 10;  // level 2 打密一点
}

void trace_body_blk(TraceTag& tag, size_t len, uint64_t beginMs) {
    if(!tag.on || !TraceGate::instance().enabled()) return;
    ++tag.blk;
    tag.bytes += len;
    uint64_t now = upMs();
    uint32_t step = TraceGate::instance().deep() ? kBlkStepDeep : kBlkStep;

    if(tag.blk == 1) {
        // 首块 = 客户端真正看到第一个字节的时刻
        tag.lastMs = now;
        GW_TRACE(tag, "cli  body#1  n=" << len << " t=" << (now - beginMs) << "ms");
        return;
    }
    if(tag.blk % step == 0 || (tag.lastMs && now - tag.lastMs >= kMsStep)) {
        tag.lastMs = now;
        GW_TRACE(tag, "cli  body~   blk=" << tag.blk << " bytes=" << tag.bytes
                      << " t=" << (now - beginMs) << "ms");
    }
}

void trace_body_end(TraceTag& tag, uint64_t beginMs, const char* how) {
    if(!tag.on || !TraceGate::instance().enabled()) return;
    GW_TRACE(tag, "cli  bodyend blk=" << tag.blk << " bytes=" << tag.bytes
                  << " cost=" << (upMs() - beginMs) << "ms " << how);
}

TraceFilter::ptr TraceGate::filter() const {
    std::lock_guard lk(m_mtx);
    return m_filter;
}

void TraceGate::open(uint8_t level, const TraceFilter& f) {
    {
        std::lock_guard lk(m_mtx);
        m_filter = std::make_shared<const TraceFilter>(f);
    }
    m_seq.store(0, std::memory_order_relaxed);
    m_level.store(level ? level : 1, std::memory_order_release);
}

void TraceGate::close() {
    m_level.store(0, std::memory_order_release);
    std::lock_guard lk(m_mtx);
    m_filter.reset();
}

bool TraceGate::enabled() {
    if(!on()) return false;
    expire_check();
    return on();
}

// ttl 到点自动关, 防开了忘关把盘写满
void TraceGate::expire_check() {
    auto f = filter();
    if(f && f->expireMs && bronx::GetCurrentMs() >= f->expireMs) {
        close();
    }
}

bool TraceGate::want(const std::string& ip, const std::string& path) {
    if(!on()) return false;
    expire_check();
    auto f = filter();
    if(!f) return on();
    if(!f->ip.empty() && f->ip != ip) return false;
    if(!f->path.empty() && path.compare(0, f->path.size(), f->path) != 0) return false;
    if(f->sample > 1) {
        uint64_t n = m_seq.fetch_add(1, std::memory_order_relaxed);
        if(n % f->sample != 0) return false;
    }
    return on();
}

} // namespace gateway
} // namespace bronx
