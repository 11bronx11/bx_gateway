#pragma once

#include <atomic>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility> 
#include <vector>
#include "mpsc_ring.h"
#include "sync.h"
#include "noncopyable.h"
#include "thread.h"
namespace bronx {   

class BxLogAppender;


// 关停由 atexit 触发 shutdown，顺序有讲究，先把 running_ 翻掉让新来的 push 改走同步写，
// 再等还在途中的 producer 把 push 跑完，然后 flushAll 把 TLS 里的残留全推进队列等 writer
// 写光，最后 stop_ 置位叫醒并 join 掉心跳和 writer，队列里万一还剩的自己同步 drain 掉。
class BxAsyncLogger : public Noncopyable {
public:
    // 队列里必须持有 shared_ptr：配置热更新 clearAppenders() 后，旧 appender
    // 仍可能有待写日志，不能只保存裸指针。
    using Item = std::pair<std::shared_ptr<BxLogAppender>, std::string>;

    // 单条 buffer 容纳的 Item 数量；buffer 自身是堆分配
    struct Buffer {
        std::vector<Item> items;
        explicit Buffer(size_t cap) { items.reserve(cap); }
    };
    using BufferPtr = std::unique_ptr<Buffer>;

    BxAsyncLogger();
    ~BxAsyncLogger();

    // 业务线程入口；FATAL 等同步路径不应调用
    void push(std::shared_ptr<BxLogAppender> dst, std::string&& formatted);

    // 强制让所有 TLS 残留进入中心队列，并等待 writer drain 完毕
    void flushAll();

    // 关停 writer 与心跳线程；幂等
    void shutdown();

    // 仅供测试 / 调优
    static size_t kBufferCapacity;       // 每个 TLS buffer 的 Item 数
    static size_t kCentralCapacity;      // 中心队列槽位数
    static unsigned kHeartbeatMs;        // 心跳周期

private:
    struct TlsState {
        BxSpinLock lock;                          // 保护 current/spare 指针交换
        BufferPtr current;
        BufferPtr spare;
        BxAsyncLogger* owner = nullptr;
        std::list<TlsState*>::iterator it{};
        bool registered = false;
    };

    // pthread TLS：键值析构时把残留刷到中心队列并摘除注册
    static void tlsDestructor(void* p);
    TlsState* getOrCreateTls();

    // 把指定 TlsState 的 current 与 spare 交换；返回交换出的非空 buffer
    BufferPtr swapBufferLocked(TlsState* s);

    // 推送 buffer 到中心队列；满时背压等待
    void enqueueBuffer(BufferPtr buf);

    void writerLoop();
    void heartbeatLoop();

    // 写出 buffer 中所有 item 到对应 appender
    void drainBuffer(Buffer& buf);

    // 把 buffer 内存归还到 free-list 复用
    void recycleBuffer(BufferPtr buf);
    BufferPtr acquireBuffer();

private:
    // 中心队列：buffer 指针所有权交接
    std::unique_ptr<BxMpscRing<BufferPtr>> queue_;

    // 注册的所有 TlsState（用于心跳和 flushAll 遍历）
    std::mutex tlsMutex_;
    std::list<TlsState*> tlsList_;

    // writer 与心跳的同步原语
    std::mutex writerMutex_;
    std::condition_variable writerCv_;

    std::mutex hbMutex_;
    std::condition_variable hbCv_;

    // 背压：writer 出队时唤醒 producer
    std::mutex pressMutex_;
    std::condition_variable pressCv_;

    // buffer 空闲链,免得老 new
    std::mutex freeMutex_;
    std::vector<BufferPtr> freeList_;

    // flushAll 完成的同步：每次 drainBuffer 完成 +1
    std::atomic<uint64_t> drainedBuffers_{0};
    std::atomic<uint64_t> enqueuedBuffers_{0};

    // in-flight producer 计数：进 push 路径 +1，出 push 路径 -1
    // shutdown 等它清零再做 flushAll，确保已 swap 出 TLS 但尚未 enqueue 的
    // buffer 不会被遗漏（否则会有日志静默丢失）
    std::atomic<int64_t> inflight_{0};

    // flushAll 等 drained 进度推进时挂在这里；writer 每处理完一条 buffer 唤醒
    std::mutex drainMutex_;
    std::condition_variable drainCv_;

    BxThread::ptr writer_;
    BxThread::ptr heartbeat_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};

    // pthread TLS key（用于 tlsDestructor）
    pthread_key_t tlsKey_{};
};


// BxAsyncLogger 是 leaky singleton：堆分配，进程退出时不 delete。
// 这样 TLS 析构器（在业务线程退出时由 pthread runtime 调用）永远能拿到
// 有效对象，避免 use-after-free。进程退出后内存由 OS 回收。
// 关停由构造函数注册的 atexit 钩子触发 shutdown() 完成刷盘和停 worker。
class BxAsyncLoggerMgr {
public:
    static BxAsyncLogger* GetInstance() {
        static BxAsyncLogger* inst = new BxAsyncLogger();
        return inst;
    }
};

}
