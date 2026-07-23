#include "thread.h"
#include "log.h"
#include "util.h"
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>


namespace bronx{

static BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

// TLS:不用拿到线程对象也能取当前线程信息
static thread_local BxThread* t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOW";
// per-thread 备用信号栈: 栈溢出型 SIGSEGV 时主栈已废, 需要独立内存才能跑 SA_ONSTACK handler
static thread_local char t_altStack[65536];


BxThread* BxThread::Current(){
    return t_thread;
}
std::string BxThread::GetName(){
    return t_thread_name;
}

void BxThread::SetName(const std::string& name){
    if(name.empty()){
        return;
    }
    if(t_thread){
        t_thread->name_ = name;
    }
    t_thread_name = name;
}


BxThread::BxThread(std::function<void()> cb, const std::string& name)
    : BxThread(cb, name, BxThreadOptions{}){
}

BxThread::BxThread(std::function<void()> cb, const std::string& name, const BxThreadOptions& opts)
    : cb_(cb)
    , name_(name)
    , opts_(opts){
    if(name.empty()){
        name_ = "UNKNOW";
    }

    // 栈大小须在 create 前经 pthread_attr 设定(其余能力在 run 里线程自身上下文里设)
    pthread_attr_t attr;
    pthread_attr_t* pattr = nullptr;
    if(opts_.stackSize > 0){
        pthread_attr_init(&attr);
        if(pthread_attr_setstacksize(&attr, opts_.stackSize) == 0){
            pattr = &attr;
        } else {
            BRONX_LOG_WARN(g_logger) << "setstacksize(" << opts_.stackSize
                                     << ") failed, using default; name=" << name_;
            pthread_attr_destroy(&attr);
        }
    }

    // 传 this 进去,run() 才能访问本对象
    int rt = pthread_create(&thread_, pattr, &BxThread::run, this);
    if(pattr){
        pthread_attr_destroy(pattr);
    }
    if(rt){
        BRONX_LOG_ERROR(g_logger) << "pthread_create fail, errnum=" << rt
                                  << " name=" << name;
        throw std::logic_error("pthread_create error");
    }
    // 等 run() 里初始化完成
    semaphore_.wait();
}

// jthread 风格:把 void(stop_token) 包成 void(),token 取自本对象的 stopSource_。
// 委托到 opts 构造:目标构造先完整初始化本对象(含 stopSource_)并起线程,lambda 在
// 新线程上跑时对象已构造完,读 stopSource_ 安全。
BxThread::BxThread(std::function<void(std::stop_token)> cb, const std::string& name,
                   const BxThreadOptions& opts)
    : BxThread([this, cb]{ cb(stopSource_.get_token()); }, name, opts){
    joinOnDestroy_ = true;
}

BxThread::~BxThread(){
    if(thread_){
        // stop_token 重载按 jthread 语义等待退出,保证回调捕获的 this/TLS 不会悬空。
        // 旧 void() 重载维持 detach;自身线程内析构也不能 join 自己。
        stopSource_.request_stop();
        if(joinOnDestroy_ && !pthread_equal(pthread_self(), thread_)){
            int rt = pthread_join(thread_, nullptr);
            if(rt){
                BRONX_LOG_WARN(g_logger) << "pthread_join in destructor failed, errnum=" << rt
                                         << " name=" << name_;
                pthread_detach(thread_);
            }
        } else {
            pthread_detach(thread_);
        }
        thread_ = 0;
    }
}

// 在目标线程自身上下文里设 affinity/priority。best-effort:失败仅告警。
void BxThread::applyCapabilities(){
    if(opts_.cpuAffinity){
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(*opts_.cpuAffinity, &set);
        if(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0){
            BRONX_LOG_WARN(g_logger) << "setaffinity(core=" << *opts_.cpuAffinity
                                     << ") failed; name=" << name_;
        }
    }
    if(opts_.priority){
        // SCHED_OTHER 的 sched_priority 只能为 0;普通线程优先级应通过 nice 设置。
        int niceValue = *opts_.priority;
        if(niceValue < -20 || niceValue > 19){
            BRONX_LOG_WARN(g_logger) << "nice(" << niceValue
                                     << ") out of range [-20,19]; name=" << name_;
        } else if(setpriority(PRIO_PROCESS, 0, niceValue) != 0){
            BRONX_LOG_WARN(g_logger) << "setpriority(nice=" << niceValue
                                     << ") failed, errno=" << errno << "; name=" << name_;
        }
    }
}


void BxThread::join(){
    if(thread_){
        int rt = pthread_join(thread_, nullptr);
        if(rt){
            BRONX_LOG_ERROR(g_logger) << "pthread_joib fail, errnum=" << rt
                                    << " name=" << name_;
            throw std::logic_error("pthread_join error");
        }
        thread_ = 0;
    }
}

void* BxThread::run(void* arg){
    BxThread* thread = (BxThread*)arg;

    t_thread = thread;
    t_thread_name = thread->name_;
    thread->id_ = bronx::GetThreadId();

    // Linux 线程名最长 16 字节(含 '\0'),截前 15 个字符
    pthread_setname_np(pthread_self(), thread->name_.substr(0, 15).c_str());

    // 在本线程上下文里应用 affinity/priority(best-effort)
    thread->applyCapabilities();

    // per-thread altstack: 给本线程装独立备用栈, 栈溢出型崩溃时能跑 SA_ONSTACK handler
    {
        stack_t ss;
        ss.ss_sp = t_altStack;
        ss.ss_size = sizeof(t_altStack);
        ss.ss_flags = 0;
        if(sigaltstack(&ss, nullptr) != 0){
            // best-effort, 失败只告警不阻塞线程启动
            BRONX_LOG_WARN(g_logger) << "sigaltstack failed, errno=" << errno
                                     << "; name=" << thread->name_;
        }
    }

    // 把 cb 搬到局部变量,避免 cb_ 被跨线程访问
    std::function<void()> cb;
    cb.swap(thread->cb_);

    // 初始化完成,放行构造函数
    thread->semaphore_.notify();

    cb();

    return 0;
}




}
