#include "async_logger.h"
#include "log.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <unistd.h>



namespace bronx {

size_t BxAsyncLogger::kBufferCapacity = 1024;
size_t BxAsyncLogger::kCentralCapacity = 1024;
unsigned BxAsyncLogger::kHeartbeatMs = 3000;


BxAsyncLogger::BxAsyncLogger() {
    queue_.reset(new BxMpscRing<BufferPtr>(kCentralCapacity));

    int rc = pthread_key_create(&tlsKey_, &BxAsyncLogger::tlsDestructor);
    if (rc != 0) {
        std::cerr << "BxAsyncLogger: pthread_key_create failed, rc=" << rc << std::endl;
        std::abort();
    }

    running_.store(true, std::memory_order_release);

    writer_.reset(new BxThread([this]{ writerLoop(); }, "log_writer"));
    heartbeat_.reset(new BxThread([this]{ heartbeatLoop(); }, "log_heartbeat"));

    // 进程退出时拉这条钩子；幂等。leaky singleton 永不析构，
    // 这里只负责 flush + 停 worker，对象本身留到进程退出由 OS 回收。
    std::atexit([]{
        BxAsyncLoggerMgr::GetInstance()->shutdown();
    });
}

BxAsyncLogger::~BxAsyncLogger() {
    // leaky singleton：实际不会被调用。保留实现以防误用 stack 实例。
    shutdown();
    pthread_key_delete(tlsKey_);
}


void BxAsyncLogger::tlsDestructor(void* p) {
    if (!p) return;
    auto* s = static_cast<TlsState*>(p);
    // leaky singleton 保证 owner 永远有效
    BxAsyncLogger* a = s->owner;

    // 把残留 buffer 交给中心队列
    BufferPtr leftover;
    {
        BxSpinLock::Lock lock(s->lock);
        if (s->current && !s->current->items.empty()) {
            leftover = std::move(s->current);
            s->current.reset();
        }
    }
    if (a && leftover) {
        a->enqueueBuffer(std::move(leftover));
    }

    // 从全局 list 摘除
    if (a) {
        std::lock_guard<std::mutex> g(a->tlsMutex_);
        if (s->registered) {
            a->tlsList_.erase(s->it);
            s->registered = false;
        }
    }

    delete s;
}


BxAsyncLogger::TlsState* BxAsyncLogger::getOrCreateTls() {
    auto* s = static_cast<TlsState*>(pthread_getspecific(tlsKey_));
    if (s) return s;

    s = new TlsState();
    s->owner = this;
    s->current = acquireBuffer();
    s->spare = acquireBuffer();

    {
        std::lock_guard<std::mutex> g(tlsMutex_);
        tlsList_.push_front(s);
        s->it = tlsList_.begin();
        s->registered = true;
    }

    pthread_setspecific(tlsKey_, s);
    return s;
}


BxAsyncLogger::BufferPtr BxAsyncLogger::acquireBuffer() {
    {
        std::lock_guard<std::mutex> g(freeMutex_);
        if (!freeList_.empty()) {
            BufferPtr b = std::move(freeList_.back());
            freeList_.pop_back();
            b->items.clear();
            return b;
        }
    }
    return BufferPtr(new Buffer(kBufferCapacity));
}


void BxAsyncLogger::recycleBuffer(BufferPtr buf) {
    if (!buf) return;
    buf->items.clear();
    std::lock_guard<std::mutex> g(freeMutex_);
    // 上限设小一些；双缓冲只会让 list 长期保持 ~ N(线程) 个 buffer，过多即丢弃
    if (freeList_.size() < 16) {
        freeList_.push_back(std::move(buf));
    }
    // 超过上限就让 unique_ptr 自然析构
}


BxAsyncLogger::BufferPtr BxAsyncLogger::swapBufferLocked(TlsState* s) {
    // 调用方已持有 s->lock
    BufferPtr out;
    if (s->current && !s->current->items.empty()) {
        out = std::move(s->current);
        if (s->spare) {
            s->current = std::move(s->spare);
            s->spare.reset();
        } else {
            // spare 空：尝试从 free-list 复用，否则 new
            s->current = acquireBuffer();
        }
    }
    return out;
}


void BxAsyncLogger::enqueueBuffer(BufferPtr buf) {
    if (!buf || buf->items.empty()) return;

    constexpr int kSpinTries = 32;
    int spins = 0;
    while (!queue_->try_push(std::move(buf))) {
        // try_push 在 CAS 失败前不会触动 cell.data，buf 仍持有原对象（详见 mpsc_ring.h）
        if (++spins < kSpinTries) {
            continue;
        }
        // 转为条件变量等待 writer 出队
        std::unique_lock<std::mutex> lk(pressMutex_);
        pressCv_.wait_for(lk, std::chrono::milliseconds(1));
        spins = 0;
    }

    // 入队成功后再计数：保证 enqueuedBuffers_ 严格反映"已在队列里的 buffer 数"，
    // flushAll 的 snapshot 不会把还在 producer 手里的 buffer 算进去。
    enqueuedBuffers_.fetch_add(1, std::memory_order_release);

    writerCv_.notify_one();
}


void BxAsyncLogger::push(std::shared_ptr<BxLogAppender> dst, std::string&& formatted) {
    // 关停后退路：直接同步写到 dst，避免 TLS 残留丢失。
    // dst 是 shared_ptr，即使配置热更新已从 BxLogger 移除 appender，这里仍安全。
    if (!running_.load(std::memory_order_acquire)) {
        if (dst) {
            dst->writeRaw(formatted);
            dst->flush();
        }
        return;
    }

    // 进入异步路径前 +1，让 shutdown 必须等所有 in-flight producer 跑完
    // 整个 push（包括出 spinlock 后的 enqueueBuffer）才能 flushAll/停 worker。
    inflight_.fetch_add(1, std::memory_order_acquire);

    // 二次确认：running_ 可能在 fetch_add 前一刻被翻成 false
    if (!running_.load(std::memory_order_acquire)) {
        inflight_.fetch_sub(1, std::memory_order_release);
        if (dst) {
            dst->writeRaw(formatted);
            dst->flush();
        }
        return;
    }

    TlsState* s = getOrCreateTls();
    BufferPtr fullOut;
    bool emplaced = false;

    {
        BxSpinLock::Lock lock(s->lock);
        if (running_.load(std::memory_order_acquire)) {
            if (!s->current) {
                s->current = acquireBuffer();
            }
            s->current->items.emplace_back(std::move(dst), std::move(formatted));
            emplaced = true;
            if (s->current->items.size() >= kBufferCapacity) {
                fullOut = swapBufferLocked(s);
            }
        }
    }

    if (!emplaced) {
        // formatted 没有被 move，仍然有效
        if (dst) {
            dst->writeRaw(formatted);
            dst->flush();
        }
        inflight_.fetch_sub(1, std::memory_order_release);
        return;
    }
    if (fullOut) {
        enqueueBuffer(std::move(fullOut));
    }
    inflight_.fetch_sub(1, std::memory_order_release);
}


void BxAsyncLogger::flushAll() {
    // 1. 把所有 TLS 的 current 强制交换出去
    std::vector<BufferPtr> swapped;
    {
        std::lock_guard<std::mutex> g(tlsMutex_);
        for (TlsState* s : tlsList_) {
            BufferPtr out;
            {
                BxSpinLock::Lock lock(s->lock);
                out = swapBufferLocked(s);
            }
            if (out) swapped.push_back(std::move(out));
        }
    }

    for (auto& b : swapped) {
        enqueueBuffer(std::move(b));
    }
    uint64_t targetEnqueued = enqueuedBuffers_.load(std::memory_order_acquire);

    // 2. 等 writer drain 到 targetEnqueued；用 condvar 替代 yield 自旋避免空转。
    //    若 stop_ 已置位，writer 即将退出，由 shutdown 末尾的同步 drain 兜底，这里直接返回。
    std::unique_lock<std::mutex> lk(drainMutex_);
    drainCv_.wait(lk, [&]{
        return drainedBuffers_.load(std::memory_order_acquire) >= targetEnqueued
            || stop_.load(std::memory_order_acquire);
    });
}


void BxAsyncLogger::shutdown() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return; // 已经停过
    }

    // 等所有 in-flight producer 跑完 push 全程（包括出 spinlock 后的 enqueueBuffer）。
    // 这样 flushAll 之后不会再有 producer 把 fullOut 推入队列。
    while (inflight_.load(std::memory_order_acquire) > 0) {
         usleep(100);
    }

    // 最后一次全员交换
    flushAll();

    // 通知心跳与 writer 退出
    stop_.store(true, std::memory_order_release);
    hbCv_.notify_all();
    writerCv_.notify_all();
    pressCv_.notify_all();
    drainCv_.notify_all();

    if (heartbeat_) {
        heartbeat_->join();
        heartbeat_.reset();
    }
    if (writer_) {
        writer_->join();
        writer_.reset();
    }

    // writer 已退出；如果还有残留（理论上 flushAll 已清空），同步 drain 一遍
    BufferPtr leftover;
    while (queue_ && queue_->try_pop(leftover)) {
        if (leftover) drainBuffer(*leftover);
        leftover.reset();
    }
}


void BxAsyncLogger::drainBuffer(Buffer& buf) {
    std::vector<std::shared_ptr<BxLogAppender>> touched;
    for (auto& item : buf.items) {
        if (item.first) {
            // 写出已格式化字符串
            try {
                // 这里用 const std::string& 接口；具体 appender 实现见 log.cpp
                item.first->writeRaw(item.second);
                bool seen = false;
                for (auto& app : touched) {
                    if (app.get() == item.first.get()) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    touched.push_back(item.first);
                }
            } catch (const std::exception& e) {
                std::cerr << "BxAsyncLogger writeRaw exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "BxAsyncLogger writeRaw unknown exception" << std::endl;
            }
        }
    }
    // 批量 flush：比 BxFileLogAppender 每条日志 flush 一次更符合异步批处理模型。
    for (auto& app : touched) {
        try {
            app->flush();
        } catch (const std::exception& e) {
            std::cerr << "BxAsyncLogger flush exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "BxAsyncLogger flush unknown exception" << std::endl;
        }
    }
    buf.items.clear();
}


void BxAsyncLogger::writerLoop() {
    while (true) {
        BufferPtr buf;
        if (queue_->try_pop(buf)) {
            if (buf) {
                drainBuffer(*buf);
                recycleBuffer(std::move(buf));
                drainedBuffers_.fetch_add(1, std::memory_order_release);

                // 唤醒 flushAll 等待者
                {
                    std::lock_guard<std::mutex> g(drainMutex_);
                }
                drainCv_.notify_all();

                // 唤醒可能在背压等待的 producer
                pressCv_.notify_all();
            }
            continue;
        }
        // 队列空：等到有新 buffer 或 stop 信号
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }
        std::unique_lock<std::mutex> lk(writerMutex_);
        writerCv_.wait_for(lk, std::chrono::milliseconds(50), [this]{
            return stop_.load(std::memory_order_acquire)
                || !queue_->empty_approx();
        });
    }
}


void BxAsyncLogger::heartbeatLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lk(hbMutex_);
            hbCv_.wait_for(lk, std::chrono::milliseconds(kHeartbeatMs), [this]{
                return stop_.load(std::memory_order_acquire);
            });
        }
        if (stop_.load(std::memory_order_acquire)) break;

        // 全员交换
        std::vector<BufferPtr> swapped;
        {
            std::lock_guard<std::mutex> g(tlsMutex_);
            for (TlsState* s : tlsList_) {
                BufferPtr out;
                {
                    BxSpinLock::Lock spLock(s->lock);
                    out = swapBufferLocked(s);
                }
                if (out) swapped.push_back(std::move(out));
            }
        }
        for (auto& b : swapped) {
            enqueueBuffer(std::move(b));
        }
    }
}

}
