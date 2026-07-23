// 协程唤醒器, 包一层 offload 那个跑通了的 hooked eventfd。
// 发送协程 wait() 挂着不空转, 别的协程/线程 notify() 把它戳醒。停机 = 置标志再 notify。
#pragma once

#include "offload.h"
#include <cerrno>
#include <system_error>

namespace bronx {
namespace ipban {

class Waker {
public:
    // 戳醒等待者。非阻塞, 任意线程/协程可调(write_f 裸写)。
    void notify() { bronx::detail::NotifyEventFd(m_ev.fd()); }

    // 挂起当前协程直到被 notify。正常唤醒返回 true; fd 被关(取消)返回 false。
    bool wait() {
        try {
            bronx::detail::WaitEventFd(m_ev.fd());
            return true;
        } catch(const std::system_error& e) {
            if(e.code().value() == ECANCELED) return false;
            throw;
        }
    }

    int fd() const { return m_ev.fd(); }

private:
    bronx::detail::EventFd m_ev;   // 析构走插桩 close, 会 abortAll 唤醒等待者
};

} // namespace ipban
} // namespace bronx
