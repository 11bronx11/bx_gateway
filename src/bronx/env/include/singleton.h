#pragma once

// 单例设施:
//  - Singleton<T>        :Meyers 局部静态单例(线程安全懒初始化,零开销)
//  - LazySingleton<T>    :std::call_once 显式初始化版(可讲清初始化时机;预留带参初始化)
//  - AtShutdown / ShutdownRegistry:按注册逆序(LIFO)在进程退出时跑清理回调,
//    解 Meyers 单例"析构顺序不可控"的坑(与 async_logger 的 atexit 收口思路统一)。
//
// 说明:去掉了旧 sylar 的三参 Singleton<T,X,N>(X/N 无人使用)与未用的 SingletonSptr。

#include <mutex>
#include <functional>
#include <vector>

namespace bronx{

// 经典 Meyers 单例:首次 GetInstance 时构造,C++11 起局部静态初始化本身线程安全。
// 适合无需控制销毁时机的进程级对象(FdMgr/LoggerMgr/EnvMgr)。
template<typename T>
class Singleton{
public:
    static T* GetInstance(){
        static T v;
        return &v;
    }
};


// call_once 显式初始化单例:初始化时机明确、可读性强,便于讲"为何线程安全"。
// 用 new 分配、进程退出不主动 delete(与 leaky-singleton 一致,避开析构顺序坑;
// 需要有序清理时配合 AtShutdown 注册)。
template<typename T>
class LazySingleton{
public:
    static T* GetInstance(){
        std::call_once(onceFlag(), []{ instance() = new T(); });
        return instance();
    }
private:
    static T*& instance(){ static T* p = nullptr; return p; }
    static std::once_flag& onceFlag(){ static std::once_flag f; return f; }
};


// 进程退出清理注册表:注册的回调在 atexit 时按 LIFO(后注册先跑)执行,
// 让互相依赖的单例能按"后建先毁"的确定顺序释放,规避静态析构顺序未定义问题。
class ShutdownRegistry{
public:
    static ShutdownRegistry& Instance(){
        static ShutdownRegistry r;
        return r;
    }

    void add(std::function<void()> fn){
        // atexit 必须在 Instance() 完整构造【之后】注册:标准保证"初始化完成后注册的
        // atexit handler 先于该静态对象析构执行",故 RunAll 跑时 registry 仍存活。
        // 若放构造函数里注册(初始化未完成),则 handler 反而晚于析构 → 访问已毁对象 UB。
        std::call_once(atexitOnce_, []{ std::atexit(&ShutdownRegistry::RunAll); });
        std::lock_guard<std::mutex> lk(mutex_);
        callbacks_.push_back(std::move(fn));
    }

private:
    ShutdownRegistry() = default;

    static void RunAll(){
        auto& self = Instance();
        std::vector<std::function<void()>> cbs;
        {
            std::lock_guard<std::mutex> lk(self.mutex_);
            cbs.swap(self.callbacks_);
        }
        for(auto it = cbs.rbegin(); it != cbs.rend(); ++it){
            if(*it) (*it)();
        }
    }

    std::once_flag atexitOnce_;
    std::mutex mutex_;
    std::vector<std::function<void()>> callbacks_;
};

// 便捷入口:注册一个进程退出清理回调(LIFO 执行)。
inline void AtShutdown(std::function<void()> fn){
    ShutdownRegistry::Instance().add(std::move(fn));
}

}
