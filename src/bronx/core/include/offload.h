#pragma once
// 把协程和 BxCpuPool 接起来的桥，纯头文件。靠一个 eventfd 通信，先建好 eventfd 交给
// hook/BxFdManager 托管，submit 出 CPU task，然后当前 fiber 用 hooked read 去等那 8 字节
// counter。task 早完了 eventfd 就已可读，read 立刻返回；没完 read 拿到 EAGAIN，hook 会
// armEvent 再 yield，把 fiber 安全挂起。
//
// eventfd 只管"完成了"这个通知，真正的 result/exception 可见性靠 state 那把 mutex 的
// unlock/lock 来建立，别拿 syscall 当共享对象的数据同步用。

#include <sys/eventfd.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>

#include "cpu_pool.h"
#include "fd_context.h"
#include "fiber.h"
#include "io_hook.h"
#include "reactor.h"

namespace bronx {
namespace detail {

enum class OffloadStatus {
    Pending,
    Completed,
    Failed,
    Cancelled
};

using OffloadWaitObserver = void(*)(int);
inline std::atomic<OffloadWaitObserver> g_offloadWaitObserver{nullptr};

inline void SetOffloadWaitObserverForTest(OffloadWaitObserver observer) {
    g_offloadWaitObserver.store(observer, std::memory_order_release);
}

class EventFd {
public:
    EventFd()
        : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "offload eventfd");
        }
        auto ctx = FdMgr::GetInstance()->get(fd_, true);
        if (!ctx || ctx->isClose()) {
            int e = errno ? errno : EBADF;
            close();
            throw std::system_error(e, std::generic_category(), "offload eventfd fdctx");
        }
        ctx->setHookNonblock(true);
        ctx->setSysNonblock(true);
        ctx->setUserNonblock(false);
    }

    ~EventFd() {
        close();
    }

    EventFd(const EventFd&) = delete;
    EventFd& operator=(const EventFd&) = delete;

    int fd() const { return fd_; }

private:
    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

template<typename T>
struct OffloadCompletion {
    EventFd event;
    std::mutex mutex;
    OffloadStatus status = OffloadStatus::Pending;
    std::optional<T> result;
    std::exception_ptr exception;
};

template<>
struct OffloadCompletion<void> {
    EventFd event;
    std::mutex mutex;
    OffloadStatus status = OffloadStatus::Pending;
    std::exception_ptr exception;
};

inline void NotifyEventFd(int fd) {
    uint64_t one = 1;
    while (true) {
        ssize_t n = write_f(fd, &one, sizeof(one));
        if (n == static_cast<ssize_t>(sizeof(one))) {
            return;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n == -1 && errno == EAGAIN) {
            // eventfd counter 已满时 fd 本身必然可读；等待侧仍能被唤醒/读到计数。
            return;
        }
        if (n == -1) {
            throw std::system_error(errno, std::generic_category(), "offload eventfd write");
        }
        throw std::runtime_error("offload eventfd write returned short count");
    }
}

inline void WaitEventFd(int fd) {
    auto observer = g_offloadWaitObserver.load(std::memory_order_acquire);
    if (observer) {
        observer(fd);
    }
    uint64_t counter = 0;
    while (true) {
        ssize_t n = ::read(fd, &counter, sizeof(counter));
        if (n == static_cast<ssize_t>(sizeof(counter))) {
            return;
        }
        if (n == -1 && errno == EINTR) {
            continue;
        }
        if (n == -1 && errno == EAGAIN) {
            // 正常在 BxIoManager hook 线程不会到这里：do_io 会 armEvent + yield。
            // 若调用环境未启用 hook，避免自旋，直接暴露调用方式错误。
            throw std::system_error(EAGAIN, std::generic_category(),
                                    "offload eventfd read would block");
        }
        if (n == -1 && errno == ECANCELED) {
            throw std::system_error(ECANCELED, std::generic_category(),
                                    "offload wait canceled");
        }
        if (n == -1) {
            throw std::system_error(errno, std::generic_category(), "offload eventfd read");
        }
        throw std::runtime_error("offload eventfd read returned short count");
    }
}

template<typename T>
[[noreturn]] inline void ThrowCancelled() {
    throw std::system_error(ECANCELED, std::generic_category(), "offload wait canceled");
}

template<typename T>
T FinishOffload(const std::shared_ptr<OffloadCompletion<T>>& state) {
    std::exception_ptr exception;
    std::optional<T> result;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status == OffloadStatus::Pending) {
            state->status = OffloadStatus::Cancelled;
        }
        switch (state->status) {
        case OffloadStatus::Completed:
            if (state->result) {
                result.emplace(std::move(*state->result));
            } else {
                exception = std::make_exception_ptr(
                    std::runtime_error("offload completed without result"));
            }
            break;
        case OffloadStatus::Failed:
            exception = state->exception;
            break;
        case OffloadStatus::Cancelled:
            break;
        case OffloadStatus::Pending:
            exception = std::make_exception_ptr(
                std::runtime_error("offload notification without completion"));
            break;
        }
    }

    if (exception) {
        std::rethrow_exception(exception);
    }
    if (result) {
        return std::move(*result);
    }
    ThrowCancelled<T>();
}

inline void FinishOffload(const std::shared_ptr<OffloadCompletion<void>>& state) {
    std::exception_ptr exception;
    bool completed = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status == OffloadStatus::Pending) {
            state->status = OffloadStatus::Cancelled;
        }
        switch (state->status) {
        case OffloadStatus::Completed:
            completed = true;
            break;
        case OffloadStatus::Failed:
            exception = state->exception;
            break;
        case OffloadStatus::Cancelled:
            break;
        case OffloadStatus::Pending:
            exception = std::make_exception_ptr(
                std::runtime_error("offload notification without completion"));
            break;
        }
    }

    if (exception) {
        std::rethrow_exception(exception);
    }
    if (completed) {
        return;
    }
    ThrowCancelled<void>();
}

template<typename T, typename FuncPtr>
void RunCpuTask(const std::shared_ptr<OffloadCompletion<T>>& state, FuncPtr func) {
    std::optional<T> localResult;
    std::exception_ptr localException;

    try {
        localResult.emplace(std::invoke(*func));
    } catch (...) {
        localException = std::current_exception();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status == OffloadStatus::Pending) {
            if (localException) {
                state->exception = localException;
                state->status = OffloadStatus::Failed;
            } else {
                try {
                    state->result.emplace(std::move(*localResult));
                    state->status = OffloadStatus::Completed;
                } catch (...) {
                    state->exception = std::current_exception();
                    state->status = OffloadStatus::Failed;
                }
            }
        }
    }

    NotifyEventFd(state->event.fd());
}

template<typename FuncPtr>
void RunCpuTask(const std::shared_ptr<OffloadCompletion<void>>& state, FuncPtr func) {
    std::exception_ptr localException;

    try {
        std::invoke(*func);
    } catch (...) {
        localException = std::current_exception();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status == OffloadStatus::Pending) {
            if (localException) {
                state->exception = localException;
                state->status = OffloadStatus::Failed;
            } else {
                state->status = OffloadStatus::Completed;
            }
        }
    }

    NotifyEventFd(state->event.fd());
}

} // namespace detail

// 把 CPU 密集活从协程挪到 BxCpuPool 上跑，挂起协程等它做完。pool 为空或不在
// BxIoManager/fiber 上下文里就直接 inline 跑；队满或停了抛 runtime_error，上层可以转 503；
// task 自己抛的异常会透传回等待的 fiber；等待中途被 abortAll/close 叫醒则抛 ECANCELED。
template<typename F>
auto offload(F&& func, BxCpuPool::ptr pool)
    -> std::invoke_result_t<F>
{
    using T = std::invoke_result_t<F>;
    static_assert(!std::is_reference_v<T>,
                  "offload task must not return a reference");

    if (!pool || !BxFiber::Current() || !BxIoManager::Current()) {
        return std::invoke(std::forward<F>(func));
    }

    auto state = std::make_shared<detail::OffloadCompletion<T>>();
    auto task = std::make_shared<std::decay_t<F>>(std::forward<F>(func));
    int fd = state->event.fd();

    bool ok = pool->trySubmit([state, task = std::move(task)]() mutable {
        try {
            if constexpr (std::is_void_v<T>) {
                detail::RunCpuTask(state, std::move(task));
            } else {
                detail::RunCpuTask<T>(state, std::move(task));
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->status == detail::OffloadStatus::Pending) {
                state->exception = std::current_exception();
                state->status = detail::OffloadStatus::Failed;
            }
        }
    });
    if (!ok) {
        throw std::runtime_error("BxCpuPool: queue full or stopped");
    }

    try {
        detail::WaitEventFd(fd);
    } catch (const std::system_error& e) {
        if (e.code().value() == ECANCELED) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->status == detail::OffloadStatus::Pending) {
                state->status = detail::OffloadStatus::Cancelled;
            }
        }
        throw;
    }

    if constexpr (std::is_void_v<T>) {
        detail::FinishOffload(state);
    } else {
        return detail::FinishOffload<T>(state);
    }
}

// 兼容旧接口。调用方必须保证 raw pool 覆盖 offload 全过程生命周期。
template<typename F>
auto offload(F&& func, BxCpuPool* pool)
    -> std::invoke_result_t<F>
{
    if (!pool) {
        return std::invoke(std::forward<F>(func));
    }
    return offload(std::forward<F>(func), BxCpuPool::ptr(pool, [](BxCpuPool*) {}));
}

template<typename F>
auto offload(F&& func)
    -> std::invoke_result_t<F>
{
    auto pool = BxCpuPool::GetDefaultPtr();
    if (!pool) {
        return std::invoke(std::forward<F>(func));
    }
    return offload(std::forward<F>(func), std::move(pool));
}

} // namespace bronx
