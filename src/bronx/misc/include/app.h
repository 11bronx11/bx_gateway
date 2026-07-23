#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include "noncopyable.h"
#include "thread.h"
namespace bronx {
class BxCpuPool;

class BxApplication : public Noncopyable {
public:
    using ptr = std::shared_ptr<BxApplication>;

    BxApplication();
    virtual ~BxApplication();

    /// 初始化:屏蔽信号→解析命令行→加载日志与配置→onInit。失败返回 false。
    bool init(int argc, char** argv);
    /// 运行:装信号线程→onStart 起服务→挂起主线程等停。返回进程退出码。
    int  run();
    /// 触发优雅停机:CAS 置位 stopping_(仅一次)→onStop→唤醒主线程。可被任意线程调用。
    void stop();
    /// 热加载配置:重新 loadConfig 成功后回调 onReload。由 SIGHUP 触发。
    void reload();

protected:
    /// 用户钩子:init 末尾调用,做配置就绪后的初始化。返回 false 中止启动。
    virtual bool onInit()   { return true; }
    /// 用户钩子:run 中调用,在此创建并启动 BxIoManager / BxTcpServer。返回 false 中止。
    virtual bool onStart()  { return true; }
    /// 用户钩子:停机时调用,在此 stop 服务、回收资源。
    virtual void onStop()   {}
    /// 停机第一阶段:业务停止接入并释放 CPU 任务生产者,默认兼容旧 onStop。
    virtual void onStopBegin() { onStop(); }
    /// CPU pool 排干后调用,业务可在此停止 IO manager。
    virtual void onStopEnd() {}
    /// 用户钩子:收到 SIGHUP 且配置重载成功后调用。
    virtual void onReload() {}

    /// 子类构造时调用,设置必须存在的框架配置文件或目录。
    void setConfigPath(std::string p) {
        configPath_ = std::move(p);
        configPathRequired_ = true;
    }

private:
    bool startCpuPool();
    void stopCpuPool();
    /// 从 configPath_(文件或目录)加载 YAML 到 BxConfig。required=false 时路径缺失仅告警。
    bool loadConfig(bool required);
    /// 进程级屏蔽 SIGINT/SIGTERM/SIGHUP;在 init 最开头执行,子线程继承掩码。
    void blockSignals();
    /// 屏蔽信号并创建专用信号线程(运行 signalLoop)。
    void installSignals();
    /// join 并释放信号线程。
    void joinSignalThread();
    /// 信号线程主体:循环 sigtimedwait 接信号,INT/FINISHED→stop、HUP→reload。
    void signalLoop();
    /// 持锁 notify_all 唤醒挂在 cv_ 上的主线程。
    void notifyStop();

    std::string configPath_{"conf/"};      ///< 配置文件/目录路径(默认 <exe>/conf/)
    bool configPathRequired_{false};        ///< 命令行、环境变量或应用默认配置均必须存在
    std::shared_ptr<BxCpuPool> cpuPool_;
    std::atomic<bool> stopping_{false};    ///< 停机标志,主线程的 cv_ 谓词
    std::mutex mutex_;                     ///< 保护 cv_
    std::condition_variable cv_;           ///< 挂起/唤醒主线程
    BxThread::ptr signalThread_;             ///< 专用信号接收线程
};

} // namespace bronx
