#pragma once

// 一条客户端连接的驱动协程，持有 socket 和 InBuf。
// process 里跑 keep-alive 循环，读请求头，交 handler 处理，回响应，再下一个。
// 头物化 body 流式，单协程独占不加锁，drain 时配合优雅停机。

#include "buf.h"
#include "http_parse.h"
#include "http_msg.h"
#include "body.h"
#include "metrics.h"
#include "trace.h"
#include <memory>
#include <string>

namespace bronx { 
class BxSocket;
namespace gateway {

class GatewayServer;

class GatewayConnection {
public:
    GatewayConnection(std::shared_ptr<bronx::BxSocket> sock, GatewayServer* server);

    // 驱动连接生命周期:keep-alive 循环处理请求,直到关闭/出错/drain。
    // onConnection 调它,返回就代表这条连接结束了。
    void process();

    // -- 供 handler 使用的接口 --
    const GwRequest::ptr& request() const { return m_request; }
    std::shared_ptr<bronx::BxSocket> socket() const { return m_sock; }
    // 客户端 IP（供中间件记日志/限流用）
    std::string peerAddr() const;
    BodyFraming req_body_framing() const { return m_reqFraming; }
    uint64_t req_clen_of() const { return m_req_clen; }
    // 读请求体(流式):handler 需要 body 时反复调用。
    BodyReader::ptr requestBody();

    // 回写一个完整响应(头 + 可选 body)。最常见的非流式响应路径。
    // 输入:rsp 头;body 字节(可空)。自动设置 Content-Length。
    int sendResponse(const GwResponse::ptr& rsp, const std::string& body = "");

    // 回写响应头(用于流式响应:之后多次 sendBodyChunk)。
    int sendResponseHead(const GwResponse::ptr& rsp);
    // 上游转发专用:回写上游响应头,保留其状态/头,仅处理与客户端的 Connection/版本。
    int sendResponseHeadRaw(const GwResponse::ptr& rsp);
    // 流式写一块 body(按 rsp 的分帧编码)。
    int sendBodyChunk(const char* data, size_t len);
    // 原始隧道已自行写出响应头时调用,只更新连接循环的响应状态。
    void mark_raw_sent(int status);
    void markWriteFail(int status = 0);
    // 取走头解析后已预读的残留字节。WebSocket/CONNECT 升级后这些字节属于原始隧道,
    // 不能留给 HTTP body/drain 逻辑吞掉。
    std::string takeBufferedInput();

    // 全链路追踪状态,中间件/上游转发共用同一个 tag(连接 id + 请求序号)
    TraceTag& trace() { return m_trace; }
    const TraceTag& trace() const { return m_trace; }
    void startTrace(const std::string& clientIp);

private:
    // 读并解析一个请求头。返回:1 成功;0 连接正常关闭;<0 错误(已回写错误响应)。
    int read_req_head();
    int sendAll(const char* data, size_t len);
    int req_body_err_status() const;
    bool force_body_err(const GwResponse::ptr& rsp, std::string* body);
    // 丢弃当前请求未读完的 body(keep-alive 复用前必须排空)。
    bool drain_req_body();
    int fillInput();
    void reset_use_round();
    void maybe_trim_buf();
    // 回写一个简单错误响应(状态码 + 关闭连接)。
    void sendError(int status);

private:
    std::shared_ptr<bronx::BxSocket> m_sock;
    GatewayServer*       m_server;
    InBuf          m_input;
    GwRequest::ptr       m_request;
    BodyReader::ptr  m_bodyReader;   // 当前请求的 body 读取器
    BodyFraming          m_reqFraming = BodyFraming::NONE;
    uint64_t             m_req_clen = 0;
    bool                 m_responded = false;   // 当前请求是否已回响应
    bool                 m_writeFailed = false;
    int                  m_lastStatus = 0;      // 当前请求最终响应状态码（metrics/日志用）
    ReqCut               m_cut = ReqCut::NONE;
    uint32_t             m_req_count = 0;
    size_t               m_peak_readable = 0;
    size_t               m_low_use_rounds = 0;
    TraceTag             m_trace;
};

} // namespace gateway
} // namespace bronx
