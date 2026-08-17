#pragma once

// 单请求全链路追踪。平时关着(零开销 一次 atomic 读), 出问题从 admin 口打开,
// 把一条请求从 accept 到回写的每一步打进独立 trace 日志, 不污染 system。
//
// 开关: POST /trace/on?level=1&ip=&path=&sample=&ttl=   POST /trace/off   GET /trace
// level 1 = 阶段里程碑 + 每个中间件一行; level 2 = 加进入行/每次 recv/更密 body 采样。

#include "log.h"
#include "body.h"
#include "metrics.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace bronx {
namespace gateway {

// 过滤条件, 只 trace 关心的那些请求。空字段 = 不限。
struct TraceFilter {
    using ptr = std::shared_ptr<const TraceFilter>;
    std::string ip;         // 客户端 IP 精确匹配
    std::string path;       // 请求路径前缀
    uint32_t    sample = 1; // 1/N 采样, 1 = 全要
    uint64_t    expireMs = 0; // 到点自动关, 0 = 不过期
};

class TraceGate {
public:
    static TraceGate& instance() { static TraceGate g; return g; }

    // 热路径快判: 关着时就这一次 atomic 读
    bool on() const { return m_level.load(std::memory_order_relaxed) != 0; }
    bool deep() const { return m_level.load(std::memory_order_relaxed) >= 2; }
    uint8_t level() const { return m_level.load(std::memory_order_relaxed); }

    // 已命中过滤的在途请求也要受关闭和 TTL 约束。
    bool enabled();

    // 这条请求要不要 trace。链前调一次, 结果存 ctx, 后面的打点只读标志位。
    bool want(const std::string& ip, const std::string& path);

    void open(uint8_t level, const TraceFilter& f);
    void close();
    TraceFilter::ptr filter() const;

    // 到点了就关。热路径由 want 顺手调; admin 查状态前也得调一次, 否则过期后
    // 若一直没请求进来, 就没人触发过期, 状态会一直报 on(查状态的人正是想确认
    // 关没关, 撒谎最坑)。
    void expire_check();

    // trace 专用 logger, 独立文件异步写, 不挂 stdout
    static bronx::BxLogger::ptr& logger();

private:
    TraceGate() = default;

    std::atomic<uint8_t>  m_level{0};
    std::atomic<uint64_t> m_seq{0};      // 采样计数
    mutable std::mutex    m_mtx;
    TraceFilter::ptr      m_filter;
};

// 挂在连接上的追踪状态。id 是连接级序号, req 是 keep-alive 内的请求序号,
// 合起来 [c17#3] 就是一条请求的唯一标识。
struct TraceTag {
    uint64_t id  = 0;
    uint32_t req = 0;
    bool     on  = false;   // 本请求是否过了过滤
    // 连接级开关: conn open/close 和读头 recv 发生在 path 已知之前, 没法按 path 过滤,
    // 只能整条连接一起放开或关掉。ip 过滤仍生效(peer 建连时就知道)。
    bool     connOn = false;

    // body 流式采样状态
    uint32_t blk   = 0;
    uint64_t bytes = 0;
    uint64_t lastMs = 0;

    // 行序号。异步日志按线程双缓冲各自 flush, 一条请求跨线程(N:M 调度 fiber 会换 worker)
    // 时落盘顺序会乱, 时间戳只到秒也排不出来。给每行编号, sort -t. -k2 -n 就能还原。
    uint32_t seq = 0;

    void reset(uint32_t r) {
        req = r;
        on = false;
        connOn = false;
        blk = 0;
        bytes = 0;
        lastMs = 0;
    }
    // 请求级打点用 on, 连接级打点用 connOn
    bool any() const { return on || connOn; }
    std::string pfx();       // "[c17#3.007] " 请求级
    std::string pfxConn();   // "[c17.003] "   连接级(open/close, 不带请求号)
    static uint64_t nextId();
};

// body 分帧方式的短名, 日志用
const char* framingName(BodyFraming f);
// 请求被截断的原因短名
const char* reqCutName(ReqCut c);

// 打点宏。关着时成本 = 一次 atomic 读 + 一次 bool 读, 时钟都不取。
// tag 是 TraceTag&, 必须已过 want() 判定。
// 流式 body 的块采样。SSE 长回答一次上千块, 逐块打会把盘和 reactor 一起拖死。
// 策略: 首块必打(带首字节延迟) + 中间每 kBlkStep 块或 kMsStep 毫秒打一条聚合 + 末块汇总。
// beginMs 是 body 开始时刻(upMs() 口径), 用来算相对时间。
void trace_body_blk(TraceTag& tag, size_t len, uint64_t beginMs);
// body 收尾, 打总块数/总字节/总耗时
void trace_body_end(TraceTag& tag, uint64_t beginMs, const char* how);

// 变参是因为流式表达式里会出现 duration<double, std::milli> 这种带逗号的模板实参,
// 定参宏会被预处理器按逗号劈开。
#define GW_TRACE(tag, ...) \
    if(!((tag).on && bronx::gateway::TraceGate::instance().enabled())) { } \
    else BRONX_LOG_INFO(bronx::gateway::TraceGate::logger()) \
        << (tag).pfx() << __VA_ARGS__

// 连接级打点(open/close/读头 recv), 不受 path 过滤, 前缀不带请求号
#define GW_TRACE_C(tag, ...) \
    if(!((tag).connOn && bronx::gateway::TraceGate::instance().enabled())) { } \
    else BRONX_LOG_INFO(bronx::gateway::TraceGate::logger()) \
        << (tag).pfxConn() << __VA_ARGS__

// level 2 才打的细节(每次 recv / 中间件进入行 / 密集 body 采样)
#define GW_TRACE2(tag, ...) \
    if(!((tag).any() && bronx::gateway::TraceGate::instance().enabled() \
         && bronx::gateway::TraceGate::instance().deep())) { } \
    else BRONX_LOG_INFO(bronx::gateway::TraceGate::logger()) << (tag).pfx() << __VA_ARGS__

} // namespace gateway
} // namespace bronx
