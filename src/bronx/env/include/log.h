# pragma once

#include <iostream>
#include <string>
#include <vector>
#include <list>
#include <map>
#include <functional>
#include <memory>
#include <sstream>
#include <fstream>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <utility>
#include "sync.h"
#include "util.h"
#include "singleton.h"

namespace bronx{

// 日志层:分级别、多路输出、pattern 格式化,配置一改能热更新。
// 一行日志 -> BxLogEvent,一个具名日志器 -> BxLogger。任意线程随时可用。
// 写日志走流式宏 BRONX_LOG_DEBUG(logger) << ...:先按级别短路,够级别才建 event,
// BxLogEventWrap 析构时才真正提交。appender 标了 async 就走异步落盘,FATAL 强制同步。

// 日志级别
class BxLogLevel{
public:
    enum Level{
        UNKNOW = 0,     // 未知级别
        DEBUG = 1,      // DEBUG级别
        INFO = 2,       // INFO级别
        WARN = 3,       // WARN级别
        ERROR = 4,      // ERROR级别
        FATAL = 5,      // FATAL级别
        NOTSET = 100    // NOSET级别，表示不可访问
    };

    static const char* ToString(Level level);

    static Level FromString(const std::string& str);
};


// 一行日志的现场快照:发生时的所有上下文(logger 名、级别、文件行号、线程/协程 id、
// 时间等)加正文,打包成一个事件。日志宏在调用点现采这些字段,正文靠 getSS() 流式拼。
class BxLogEvent{
public:
    using ptr = std::shared_ptr<BxLogEvent>;

    BxLogEvent(std::string loggerName, BxLogLevel::Level level, const char *file, int32_t line
        , int64_t elapse, uint32_t thread_id, uint64_t fiber_id, time_t time
        , const std::string &thread_name);

    std::string getFile() const { return file_; }

    int32_t getLine() const { return line_; }

    uint32_t getElapse() const { return elapse_; }

    uint32_t getThreadId() const { return threadId_; }

    uint32_t getFiberId() const { return fiberId_; }

    uint64_t getTime() const { return time_; }

    const std::string& getThreadName() const { return threadName_; }

    std::string getContent() const { return ss_.str(); }

    std::stringstream& getSS() { return ss_; }

    std::string getLoggerName() const { return loggerName_; }

    BxLogLevel::Level getLevel() const { return level_; }

    // 以格式化方式将内容写入m_ss
    template <typename... Args>
    void format(const char* fmt, Args&&... args){
        // 先探长度再分配缓冲
        int len = snprintf(nullptr, 0, fmt, args...);
        if(len >= 0){
            char* buf = new char[len + 1];
            snprintf(buf, len + 1, fmt, args...);
            ss_ << std::string(buf, len);
            delete[] buf;
        }
    }

private:
    std::string loggerName_;           // 日志器
    BxLogLevel::Level level_;            // 日志级别
    const char* file_ = nullptr;       // 文件名
    int32_t line_ = 0;                 // 行号
    uint32_t elapse_ = 0;              // 程序启动到现在的毫秒数
    uint32_t threadId_ = 0;            // 线程ID
    uint32_t fiberId_ = 0;             // 协程ID
    uint64_t time_ = 0;                // 时间戳
    std::string threadName_;           // 线程名称
    std::stringstream ss_;             // 日志内容流
};


// 格式器,把 event 按 pattern 渲染成一行文本。
// init() 先把 "%d{...}%t...%m%n" 这样的 pattern 解析成一串 FormatItem
// (%m 正文、%p 级别、%d 时间、%t 线程…),format() 时挨个执行拼出整行。
// pattern 写错就置 error_ 并在输出里插个错误标记。时间用 localtime_r,避开静态缓冲竞争。
class BxLogFormatter{
public:
    using ptr = std::shared_ptr<BxLogFormatter>;

    BxLogFormatter(const std::string& pattern = "%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");

    virtual ~BxLogFormatter() = default;

    // 把 event 渲染成一行文本。虚函数:派生类(如 BxJsonFormatter)可整体改写渲染方式。
    virtual std::ostream& format(std::ostream& os, BxLogEvent::ptr event);

    bool isError() const { return error_; }

    std::string getPattern() const { return pattern_; }

protected:
    // 供派生类使用:不解析 pattern(派生类自有渲染逻辑,不走 items_ 那套)。
    struct NoInit{};
    explicit BxLogFormatter(NoInit) : error_(false) {}

private:
    void init();

public:
    // 抽象基类FormatItem，用于格式化某一项日志内容
    class FormatItem{
    public:
        using ptr = std::shared_ptr<FormatItem>;

        virtual ~FormatItem(){};

        virtual void format(std::ostream& os, BxLogEvent::ptr event) = 0;
    };

protected:
    std::string pattern_;
    std::vector<FormatItem::ptr> items_;
    bool error_;
};


// 结构化 JSON 格式器。把 event 渲染成一行紧凑 JSON 对象(而非 pattern 文本),
// 下游 ELK/Loki/jq 免解析直接摄取。字段固定:time/level/logger/thread/thread_name/
// fiber/file/line/msg。所有字符串值都过 JSON 转义(引号/反斜杠/控制字符),不引 jsoncpp
// 依赖(env/core 层保持零 JSON 库依赖,手写转义足够且更快)。
// 通过 YAML 里 appender 的 format: json 选择,与 pattern formatter 平级替换。
class BxJsonFormatter : public BxLogFormatter{
public:
    using ptr = std::shared_ptr<BxJsonFormatter>;

    BxJsonFormatter() : BxLogFormatter(NoInit{}) {}

    std::ostream& format(std::ostream& os, BxLogEvent::ptr event) override;
};


// 日志出口的抽象基类。承载 formatter 和同步/异步开关,派生类(控制台/文件)
// 只管实现 writeRaw/flush 这两个真正把字符串写出去的原语。
// 标了 async 且非 FATAL,就把 (shared_ptr<this>, 字符串) 推给 writer 线程异步落盘,
// 否则当场同步写。继承 enable_shared_from_this 是因为异步队列得攥着 shared_ptr,
// 防热更新 clearAppenders 后 writer 用到已经析构的 appender。
class BxLogAppender: public std::enable_shared_from_this<BxLogAppender>{
public:
    using ptr = std::shared_ptr<BxLogAppender>;
    // 用互斥量而非自旋锁:writeRaw 在锁内做可能阻塞的文件/终端 IO(filestream_.write /
    // reopen 的 close+open),自旋锁护慢 IO 是反模式——持锁线程陷入 syscall 时,其它
    // 争锁线程空转烧 CPU。Logger/Manager 的锁只护快操作(指针/容器),仍用 BxSpinLock。
    using MutexType = BxMutex;

    BxLogAppender(BxLogFormatter::ptr defaultFormatter)
        : defaultFormatter_(defaultFormatter){};

    virtual ~BxLogAppender(){};

    // 同步入口；FATAL 级别强制走同步分支
    virtual void log(BxLogEvent::ptr event);

    // 仅由 BxAsyncLogger 的 writer 线程调用：写出已格式化好的字符串
    virtual void writeRaw(const std::string& s) = 0;

    // 刷新输出缓冲。异步 writer 会在每个 buffer 写完后批量 flush；
    // 同步/FATAL 路径写完后立即 flush。
    virtual void flush() {}

    void setFormatter(BxLogFormatter::ptr formatter);

    BxLogFormatter::ptr getFormatter() const ;

    virtual std::string toYamlString() const = 0;

    void setAsync(bool v) { async_.store(v, std::memory_order_release); }
    bool isAsync() const { return async_.load(std::memory_order_acquire); }

protected:
    // Logger在调用Appender之前已经判断了日志级别，这里不需要再重复判断level
    // BxLogLevel::Level level_;
    BxLogFormatter::ptr formatter_;          // 自定义formatter
    BxLogFormatter::ptr defaultFormatter_;   // 默认formatter
    mutable MutexType mutex_;
    std::atomic<bool> async_{false};       // 是否走异步路径，配置热更新时会被其它线程读取
};

// 输出到控制台的Appender
// writeRaw 写 std::cout(自带内部锁,这里再加 BxMutex 串行化本 appender)。
class BxStdoutLogAppender: public BxLogAppender{
public:
    BxStdoutLogAppender();

    using ptr = std::shared_ptr<BxStdoutLogAppender>;

    void writeRaw(const std::string& s) override;

    void flush() override;

    std::string toYamlString() const override;
};


// 输出到文件的Appender
// writeRaw 写文件流,并每 3 秒 reopen 一次(防文件被外部删除/轮转后仍写旧句柄)。
class BxFileLogAppender: public BxLogAppender{
public:
    using ptr = std::shared_ptr<BxFileLogAppender>;

    BxFileLogAppender(const std::string& filename);

    void log(BxLogEvent::ptr event) override;

    void writeRaw(const std::string& s) override;

    void flush() override;

    std::string toYamlString() const override;

private:
    // 重新打开文件（如果文件是打开状态，则先关闭它再打开）
    // 打开成功返回true，否则返回false
    bool reopen();

private:
    std::string filename_;         // 输出目的地的文件名
    std::ofstream filestream_;     // 输出目的地的文件流
    uint64_t lastTime_ = 0;        // 上次打开时间
};


class BxLogManager;

// 令牌桶日志限流器:防日志风暴打爆磁盘/IO(高并发网关每秒几万条 access log 全量落盘是灾难)。
// 每秒最多放行 rate 条,桶容量 = rate(允许瞬时突发到一整秒的量),超出直接丢并计数。
// rate == 0 表示不限流(默认),allow() 恒真。
//
// 关键取舍:
// - 补充用 steady_clock 单调时钟,不用墙钟(gettimeofday 会因 NTP/闰秒回跳,令桶时间倒流)。
// - 临界区是纯算术(读时钟 + 加减 token),用短 spinlock 正当;这和"spinlock 护慢磁盘 IO"
//   的反模式是两码事——那里锁内是可能阻塞的 syscall,这里锁内是几十纳秒的定长运算。
// - ERROR/FATAL 的豁免不在这里判(限流器只认速率),由 BxLogger::log 在调用前按级别短路,
//   职责单一:限流器只管"这一秒还有没有配额"。
class BxLogRateLimiter{
public:
    BxLogRateLimiter() = default;

    // 设置每秒放行条数上限;0 = 不限流。线程安全,热更新可随时改。
    void setRate(uint64_t ratePerSec);
    uint64_t getRate() const { return rate_.load(std::memory_order_acquire); }

    // 尝试取一个令牌:有配额返回 true(放行),否则返回 false(丢弃)并累加 dropped_。
    bool allow();

    // 被限流丢弃的累计条数(可观测:暴露给 stats/诊断,不自动写日志以免递归)。
    uint64_t getDropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> rate_{0};        // 每秒配额;0=不限流
    std::atomic<uint64_t> dropped_{0};     // 累计被丢弃条数
    BxSpinLock mutex_;                     // 只护下面两个桶状态(纯算术临界区)
    double tokens_ = 0.0;                  // 当前令牌数
    uint64_t lastRefillNs_ = 0;            // 上次补充时刻(steady_clock 纳秒)
};

// 具名日志器:一个级别门限 + 一组 appender。未单独配 level 时跟随 root,配了后用自己的。
// 自己没挂 appender 就回退到 root_ 的。热更新时 log.cpp 的 LogIniter 会对它
// setLevel/clearAppenders/addAppender 重建。
// log() 只在锁内拷一份 appender 快照,格式化和落盘都放锁外做,持锁尽量短。
// 采样:非 ERROR/FATAL 的低级别日志过令牌桶限流(sampleRate_,0=不限),防风暴。
class BxLogger{
public:
    friend BxLogManager;

    using ptr = std::shared_ptr<BxLogger>;
    using MutexType = BxSpinLock;

    BxLogger(const std::string& name = "root");

    void log(BxLogEvent::ptr event);

    void addAppender(BxLogAppender::ptr appender);
    void delAppender(BxLogAppender::ptr appender);
    void clearAppenders();

    BxLogLevel::Level getLevel() const {
        if(inheritRootLevel_.load(std::memory_order_acquire) && root_) {
            return root_->getLevel();
        }
        return level_.load(std::memory_order_acquire);
    }
    void setLevel(BxLogLevel::Level level);

    // 采样限流:每秒最多放行 rate 条低级别日志(0=不限);ERROR/FATAL 永远豁免。
    void setSampleRate(uint64_t ratePerSec) { sampler_.setRate(ratePerSec); }
    uint64_t getSampleRate() const { return sampler_.getRate(); }
    uint64_t getSampledDropped() const { return sampler_.getDropped(); }

    std::string getName() const { return name_; }

    std::string toYamlString() const;

private:
    // 日志器的名称
    std::string name_;
    // 日志级别，只有高于该级别的日志事件才会被输出
    std::atomic<BxLogLevel::Level> level_;
    // 日志输出地的集合
    std::list<BxLogAppender::ptr> appenders_;
    // 主日志器，当 logger 没有定义 appender 时回退输出,默认 level 也随它走
    BxLogger::ptr root_;
    std::atomic<bool> inheritRootLevel_{false};
    // 采样限流器:非 ERROR/FATAL 日志过令牌桶(rate=0 时直通)
    BxLogRateLimiter sampler_;
    // 锁
    mutable MutexType mutex_;
};


// 裹住 logger 和 event,析构时才提交(RAII)。日志宏靠它把一行流式拼完再落。
class BxLogEventWrap{
public:
    BxLogEventWrap(BxLogger::ptr logger, BxLogEvent::ptr event);

    ~BxLogEventWrap();

    BxLogEvent::ptr getEvent() const { return event_; }

    std::stringstream& getSS();

private:
    BxLogger::ptr logger_;
    BxLogEvent::ptr event_;
};


// 把 ostream& 折叠成 void，配合 ?: 实现安全的级别短路
class LogVoidify {
public:
    LogVoidify() = default;
    void operator&(std::ostream&) {}
};


// 全进程日志器注册表,经 LoggerMgr 单例暴露,是拿 BxLogger 的唯一入口。
// 构造时就建好 root(挂个控制台 appender)。getLogger 已有就返回,没有就现建一个,
// 并把它的 root_ 指到本管理器的 root(于是没配 appender 的 logger 自动回退)。
// 单例进程级存活,main 前后任意时刻都能取。
class BxLogManager{
public:
    friend Singleton<BxLogManager>;

    using MutexType = BxSpinLock;

    BxLogger::ptr getLogger(std::string loggerName);

    // init实现从配置文件中加载日志配置
    void init();

    BxLogger::ptr getRoot() const { return root_; }

    std::string toYamlString() const;

private:
    // 单例模式，私有的构造函数
    BxLogManager();

private:
    // 主日志器
    BxLogger::ptr root_;
    // 管理包括m_root在内所有日志器的容器
    std::map<std::string, BxLogger::ptr> loggers_;
    // 只护 loggers_ map 的快查找/插入,用自旋锁
    mutable MutexType mutex_;
};

// 使用单例模式管理LogManager
using LoggerMgr = Singleton<BxLogManager>;

}

// 使用流式方式将日志级别level的日志写入logger
// 使用 ?: + LogVoidify 折叠避免悬空 else 陷阱
#define BRONX_LOG_LEVEL(logger, level) \
    !(logger->getLevel() <= level) ? (void)0 : bronx::LogVoidify() & \
        bronx::BxLogEventWrap(logger, std::make_shared<bronx::BxLogEvent>(logger->getName(), level, __FILE__, __LINE__\
                            , 0, bronx::GetThreadId(), bronx::CurrentId(), time(0), bronx::GetThreadName())).getSS()

#define BRONX_LOG_DEBUG(logger) BRONX_LOG_LEVEL(logger, bronx::BxLogLevel::DEBUG)

#define BRONX_LOG_INFO(logger) BRONX_LOG_LEVEL(logger, bronx::BxLogLevel::INFO)

#define BRONX_LOG_WARN(logger) BRONX_LOG_LEVEL(logger, bronx::BxLogLevel::WARN)

#define BRONX_LOG_ERROR(logger) BRONX_LOG_LEVEL(logger, bronx::BxLogLevel::ERROR)

#define BRONX_LOG_FATAL(logger) BRONX_LOG_LEVEL(logger, bronx::BxLogLevel::FATAL)


namespace bronx {

// 格式化日志的真实实现。调用点文件名/行号由下面的宏传入；
// 不能在这个 inline 函数里直接写 __FILE__/__LINE__，否则会永远指向 log.h。
template<typename... Arg>
inline void LogFmtLevel(BxLogger::ptr logger, BxLogLevel::Level level,
                        const char* file, int32_t line,
                        const char* fmt, Arg&&... args){
    if(logger->getLevel() <= level){
        BxLogEventWrap(logger, std::make_shared<BxLogEvent>(logger->getName(), level, file, line
                            , 0, GetThreadId(), CurrentId(), time(0), GetThreadName())).getEvent()->format(fmt, std::forward<Arg>(args)...);
    }
}

}

#define BRONX_LOG_FMT_LEVEL(logger, level, fmt, ...) \
    bronx::LogFmtLevel(logger, level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define BRONX_LOG_FMT_DEBUG(logger, fmt, ...) \
    BRONX_LOG_FMT_LEVEL(logger, bronx::BxLogLevel::DEBUG, fmt, ##__VA_ARGS__)

#define BRONX_LOG_FMT_INFO(logger, fmt, ...) \
    BRONX_LOG_FMT_LEVEL(logger, bronx::BxLogLevel::INFO, fmt, ##__VA_ARGS__)

#define BRONX_LOG_FMT_WARN(logger, fmt, ...) \
    BRONX_LOG_FMT_LEVEL(logger, bronx::BxLogLevel::WARN, fmt, ##__VA_ARGS__)

#define BRONX_LOG_FMT_ERROR(logger, fmt, ...) \
    BRONX_LOG_FMT_LEVEL(logger, bronx::BxLogLevel::ERROR, fmt, ##__VA_ARGS__)

#define BRONX_LOG_FMT_FATAL(logger, fmt, ...) \
    BRONX_LOG_FMT_LEVEL(logger, bronx::BxLogLevel::FATAL, fmt, ##__VA_ARGS__)


// 获取LogManager中的主日志器（root）
#define BRONX_LOG_ROOT() bronx::LoggerMgr::GetInstance()->getRoot()
// 根据名字获取logger
#define BRONX_LOG_NAME(name) bronx::LoggerMgr::GetInstance()->getLogger(name)
