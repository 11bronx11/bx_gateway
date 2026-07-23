// 空队列就睡 入队再唤醒
#pragma once

#include "rule.h"
#include "waker.h"
#include "net_socket.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace bronx {
class BxIoManager;
class BxTimer;
namespace ipban {

struct ReportOpts {
    std::string submitPath = "/tmp/bronx_ip_submit.sock";
    size_t maxQueue = 4096;
    uint64_t minBackoffMs = 200;
    uint64_t maxBackoffMs = 5000;
    uint64_t stopTimeoutMs = 2000;
};

class Reporter : public std::enable_shared_from_this<Reporter> {
public:
    using ptr = std::shared_ptr<Reporter>;
    Reporter(const ReportOpts& opts) : m_opts(opts) {}

    bool tryReport(const Risk& r);
    void start(bronx::BxIoManager* iom);
    void stop();
    bool waitStop(uint64_t ms);
    size_t queueDepth();

private:
    void loop();
    bool drainOnce(const bronx::BxSocket::ptr& sock);
    bool waitBackoff(uint64_t ms);
    bool stopDue() const;
    void dropRest();
    void finish();

    ReportOpts m_opts;
    std::mutex m_mtx;
    std::deque<Risk> m_queue;
    std::unordered_map<std::string, uint64_t> m_seen;
    Waker m_waker;

    std::atomic<bool> m_accepting{true};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_done{false};
    std::atomic<uint64_t> m_stopAt{0};
    bronx::BxIoManager* m_iom = nullptr;

    std::mutex m_sockMtx;
    bronx::BxSocket::ptr m_sock;
    std::mutex m_timerMtx;
    std::shared_ptr<bronx::BxTimer> m_backoffTimer;
    std::mutex m_doneMtx;
    std::condition_variable m_doneCv;
};

} // namespace ipban
} // namespace bronx
