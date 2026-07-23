#pragma once


#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <pthread.h>
#include "noncopyable.h"
#include "sync.h"


namespace bronx{

// 线程能力选项(全可选,默认=旧行为)。Linux 特有,均 best-effort:设不上只告警不崩。
struct BxThreadOptions{
    std::optional<int> cpuAffinity;   // 绑到指定 CPU 核(pthread_setaffinity_np)
    std::optional<int> priority;      // Linux nice 值[-20,19],负值需 CAP_SYS_NICE
    size_t             stackSize = 0; // 线程栈大小,0=系统默认(pthread_attr_setstacksize)
};

// pthread 的 RAII 封装,框架的工作线程载体。BxScheduler 用它建 worker 池。
// 构造里用信号量跟 run() 握手:等新线程初始化完(拿到内核 id、设好 TLS 和线程名)才返回,
// 所以构造一返回 getId() 就可用。不可拷贝;stop_token 重载析构时会请求停止并 join,
// 旧 void() 重载保持未 join 时 detach 的兼容行为。
class BxThread: public Noncopyable{
public:
    using ptr = std::shared_ptr<BxThread>;

    BxThread(std::function<void()> cb, const std::string& name);
    // 带能力选项的重载(affinity/priority/stackSize),旧构造签名保持不变
    BxThread(std::function<void()> cb, const std::string& name, const BxThreadOptions& opts);
    // jthread 风格:cb 首参收本线程的 stop_token,协作取消无需外部再取(避免拿不到 this 的窘境)
    BxThread(std::function<void(std::stop_token)> cb, const std::string& name,
             const BxThreadOptions& opts = {});
    ~BxThread();

    pid_t getId() const { return id_; }
    std::string getName() const { return name_; }

    // C++20 协作式取消:新代码可在线程体里查 token 主动退出。
    // 老路径(scheduler/async_logger 各有 atomic 停机)不依赖它,保持不动。
    std::stop_token getStopToken() { return stopSource_.get_token(); }
    bool requestStop() { return stopSource_.request_stop(); }

    void join();

    // 当前线程的 BxThread*(取自 TLS,主线程或非本类建的线程为 nullptr)
    static BxThread* Current();
    static std::string GetName();
    static void SetName(const std::string& name);

    // pthread 入口蹦床:填 TLS/内核 id/线程名 -> 放握手信号量 -> 跑 cb
    static void* run(void* arg);

private:
    // 在目标线程上按 opts_ 应用 affinity/priority(best-effort)
    void applyCapabilities();

    pid_t id_ = -1;            // 内核线程 id
    pthread_t thread_ = 0;     // pthread 句柄
    std::function<void()> cb_;
    std::string name_;
    BxThreadOptions opts_;     // 线程能力选项(默认空=旧行为)
    std::stop_source stopSource_;  // C++20 协作式取消源
    BxSemaphore semaphore_;    // 与 run() 的启动握手
    bool joinOnDestroy_ = false; // stop_token 重载按 jthread 语义析构等待
};




}
