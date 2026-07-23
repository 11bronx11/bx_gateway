// cpu_pool.cpp
#include "cpu_pool.h"
#include "log.h"
#include <pthread.h>
#include <thread>
#include <algorithm>
#include <stdexcept>
#include <chrono>

static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

namespace bronx {

// 全局默认实例
static BxCpuPool::ptr s_default;
static std::mutex   s_defaultMutex;

BxCpuPool::ptr BxCpuPool::GetDefaultPtr() {
    std::lock_guard<std::mutex> lk(s_defaultMutex);
    return s_default;
}

BxCpuPool* BxCpuPool::GetDefault() {
    std::lock_guard<std::mutex> lk(s_defaultMutex);
    return s_default.get();
}

void BxCpuPool::SetDefault(BxCpuPool::ptr pool) {
    BxCpuPool::ptr old;
    {
        std::lock_guard<std::mutex> lk(s_defaultMutex);
        old.swap(s_default);
        s_default = std::move(pool);
    }
    if (!old) {
        return;
    }

    auto self = std::this_thread::get_id();
    bool release_on_worker = std::any_of(old->threads_.begin(), old->threads_.end(),
        [self](const std::thread& t) { return t.get_id() == self; });
    if (release_on_worker) {
        std::thread([old = std::move(old)]() mutable { old.reset(); }).detach();
    }
}

BxCpuPool::BxCpuPool() : BxCpuPool(BxConfig{}) {}

BxCpuPool::BxCpuPool(BxConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.threads == 0) {
        size_t hw = std::thread::hardware_concurrency();
        cfg_.threads = std::max<size_t>(1, hw / 2);
    }
    threads_.reserve(cfg_.threads);
    try {
        for (size_t i = 0; i < cfg_.threads; ++i) {
            threads_.emplace_back(&BxCpuPool::workerLoop, this, i);
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        spaceCv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        throw;
    }
    BRONX_LOG_DEBUG(g_logger) << "BxCpuPool[" << cfg_.name
                              << "] started threads=" << cfg_.threads;
}

BxCpuPool::~BxCpuPool() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    spaceCv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

bool BxCpuPool::submit(std::function<void()> task) {
    return trySubmit(std::move(task));
}

bool BxCpuPool::trySubmit(std::function<void()> task) {
    if (!task) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return submitImpl(Task(std::move(task)), std::chrono::milliseconds(0), false);
}

bool BxCpuPool::submit(std::function<void()> task, std::chrono::milliseconds timeout) {
    if (!task) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return submitImpl(Task(std::move(task)), timeout, true);
}

bool BxCpuPool::submitImpl(Task task, std::chrono::milliseconds timeout, bool waitForSpace) {
    if (!task) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (stop_ || draining_.load(std::memory_order_relaxed)) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (cfg_.maxQueue > 0 && queue_.size() >= cfg_.maxQueue) {
            if (!waitForSpace || timeout <= std::chrono::milliseconds(0)) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            bool hasSpace = spaceCv_.wait_for(lk, timeout, [this] {
                return stop_
                    || draining_.load(std::memory_order_relaxed)
                    || cfg_.maxQueue == 0
                    || queue_.size() < cfg_.maxQueue;
            });
            if (!hasSpace || stop_ || draining_.load(std::memory_order_relaxed)) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        queue_.push_back(std::move(task));
        submitted_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
    return true;
}

void BxCpuPool::drain() {
    auto self = std::this_thread::get_id();
    if (std::any_of(threads_.begin(), threads_.end(),
                    [self](const std::thread& t) { return t.get_id() == self; })) {
        throw std::logic_error("BxCpuPool::drain cannot be called from a pool worker");
    }

    {
        std::unique_lock<std::mutex> lk(mutex_);
        draining_.store(true, std::memory_order_release);
        spaceCv_.notify_all();
        drainCv_.wait(lk, [this] {
            return queue_.empty()
                && active_.load(std::memory_order_acquire) == 0;
        });
        stop_ = true;
    }
    cv_.notify_all();
}

BxCpuPool::Stats BxCpuPool::getStats() const {
    Stats s;
    s.submitted = submitted_.load(std::memory_order_relaxed);
    s.completed = completed_.load(std::memory_order_relaxed);
    s.rejected  = rejected_.load(std::memory_order_relaxed);
    s.active    = active_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        s.pending = queue_.size();
    }
    return s;
}

void BxCpuPool::workerLoop(size_t id) {
    // 设置线程名（htop/gdb可见）
    std::string tname = cfg_.name + "-" + std::to_string(id);
    pthread_setname_np(pthread_self(), tname.substr(0, 15).c_str());

    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) break;
            task = std::move(queue_.front());
            active_.fetch_add(1, std::memory_order_relaxed);
            queue_.pop_front();
        }
        spaceCv_.notify_one();
        try { task(); } catch (...) {
            BRONX_LOG_ERROR(g_logger) << "BxCpuPool[" << cfg_.name
                                      << "] task threw unhandled exception";
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            active_.fetch_sub(1, std::memory_order_release);
        }
        completed_.fetch_add(1, std::memory_order_relaxed);
        drainCv_.notify_all();  // 通知drain()等待者
        spaceCv_.notify_all();
    }
}

} // namespace bronx
