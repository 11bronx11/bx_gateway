#pragma once
// 纯 CPU 工作线程池,是三层线程模型(Accept + IO 协程 + CPU)的第三层。
// 这些线程不开 hook、不持 IOManager、根本不知道协程存在,就是老老实实跑重活。
// 怎么跟协程世界搭边由 offload.h 的 offload<>() 用 eventfd 管。
#include <functional>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace bronx {

class BxCpuPool {
public:
    using ptr = std::shared_ptr<BxCpuPool>;

    struct BxConfig {
        size_t      threads  = 0;       // 0 就取 hw_concurrency 的一半,至少 1
        size_t      maxQueue = 0;       // 0 是无界,满了 submit 返回 false
        std::string name     = "cpu";   // 线程名前缀,htop/gdb 里看得到
    };

    struct Stats {
        uint64_t submitted = 0;
        uint64_t completed = 0;
        uint64_t rejected  = 0;
        size_t   pending   = 0;   // 队列里还压着多少
        size_t   active    = 0;   // 正在跑的任务数
    };

    BxCpuPool();                      // 默认配置,半数核心的线程 + 无界队列
    explicit BxCpuPool(BxConfig cfg);
    ~BxCpuPool();

    // 投递任务,线程安全。返回 false 只可能是队列满了或已经在 draining/stopped
    template<typename F>
    bool submit(F&& task) {
        return trySubmit(std::forward<F>(task));
    }

    // 非阻塞投递,队列满或已停就返回 false
    template<typename F>
    bool trySubmit(F&& task) {
        return submitImpl(Task(std::forward<F>(task)), std::chrono::milliseconds(0), false);
    }

    // 带背压的投递,timeout 内等到空位就成功,否则 false
    template<typename F>
    bool submit(F&& task, std::chrono::milliseconds timeout) {
        return submitImpl(Task(std::forward<F>(task)), timeout, true);
    }

    bool   submit(std::function<void()> task);
    bool   trySubmit(std::function<void()> task);
    bool   submit(std::function<void()> task, std::chrono::milliseconds timeout);

    // 等所有已投递任务跑完再拒新任务,析构前可选调一下
    void   drain();
    Stats  getStats()    const;
    size_t threadCount() const { return threads_.size(); }
    bool   isDraining()  const { return draining_.load(std::memory_order_relaxed); }

    // 全局默认实例（由Application::init设置；未初始化时offload降级为inline执行）
    static BxCpuPool* GetDefault();
    static BxCpuPool::ptr GetDefaultPtr();
    static void     SetDefault(BxCpuPool::ptr pool);

    // 扩展预留（接口定稿，后续按需实现）
    // bool submitWithPriority(int priority, std::function<void()> task);
    // void resize(size_t newThreads);

private:
    class Task {
    public:
        Task() = default;

        template<typename F,
                 typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, Task>>>
        explicit Task(F&& f)
            : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {
            static_assert(std::is_invocable_v<std::decay_t<F>&>,
                          "BxCpuPool task must be invocable with no arguments");
        }

        Task(Task&&) noexcept = default;
        Task& operator=(Task&&) noexcept = default;
        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        explicit operator bool() const { return static_cast<bool>(impl_); }
        void operator()() { impl_->call(); }

    private:
        struct Concept {
            virtual ~Concept() = default;
            virtual void call() = 0;
        };

        template<typename F>
        struct Model : Concept {
            template<typename Fn>
            explicit Model(Fn&& f)
                : func(std::forward<Fn>(f)) {
            }
            void call() override { std::invoke(func); }
            F func;
        };

        std::unique_ptr<Concept> impl_;
    };

    void workerLoop(size_t id);
    bool submitImpl(Task task, std::chrono::milliseconds timeout, bool waitForSpace);

    BxConfig                             cfg_;
    std::vector<std::thread>           threads_;
    std::deque<Task>                   queue_;
    mutable std::mutex                 mutex_;      // 纯std，CPU线程世界
    std::condition_variable            cv_;         // 唤醒worker
    std::condition_variable            spaceCv_;    // 队列腾出空间
    std::condition_variable            drainCv_;    // drain()等待用
    std::atomic<size_t>                active_{0};
    std::atomic<uint64_t>              submitted_{0};
    std::atomic<uint64_t>              completed_{0};
    std::atomic<uint64_t>              rejected_{0};
    std::atomic<bool>                  draining_{false};
    bool                               stop_{false};
};

} // namespace bronx
