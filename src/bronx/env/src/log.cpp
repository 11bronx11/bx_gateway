#include <set>
#include <cstring>
#include <chrono>
#include <filesystem>
#include "log.h"
#include "config.h"
#include "async_logger.h"
namespace bronx{


const char* BxLogLevel::ToString(Level level){
    switch (level){
#define XX(name) case BxLogLevel::name: return #name;
        XX(DEBUG);
        XX(INFO);
        XX(WARN);
        XX(ERROR);
        XX(FATAL);
#undef XX
        default:
            return "UNKNOW";
    }
}

BxLogLevel::Level BxLogLevel::FromString(const std::string& str){
    std::string strlower(str.size(), '\0');
    std::transform(str.begin(), str.end(), strlower.begin(), ::tolower);
#define XX(level, v) if(strlower == #v){ return BxLogLevel::level; }
    XX(DEBUG, debug);
    XX(INFO, info);
    XX(WARN, warn);
    XX(ERROR, error);
    XX(FATAL, fatal);
#undef XX
    return UNKNOW;
}


BxLogEvent::BxLogEvent(std::string loggerName, BxLogLevel::Level level, const char *file, int32_t line
        , int64_t elapse, uint32_t thread_id, uint64_t fiber_id, time_t time
        , const std::string &thread_name)
    : loggerName_(loggerName) 
    , level_(level)
    , file_(file)
    , line_(line)
    , elapse_(elapse)
    , threadId_(thread_id)
    , fiberId_(fiber_id)
    , time_(time)
    , threadName_(thread_name) {
}


// ---- 令牌桶限流器 ----
void BxLogRateLimiter::setRate(uint64_t ratePerSec){
    rate_.store(ratePerSec, std::memory_order_release);
    // 改速率时重置桶:桶初值给满(允许立刻突发一整秒的量),避免刚配好就把积压全丢。
    BxSpinLock::Lock lock(mutex_);
    tokens_ = (double)ratePerSec;
    lastRefillNs_ = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool BxLogRateLimiter::allow(){
    uint64_t rate = rate_.load(std::memory_order_acquire);
    if(rate == 0){
        return true;   // 0 = 不限流,直通
    }
    uint64_t nowNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    BxSpinLock::Lock lock(mutex_);
    // 按流逝时间补充令牌:rate 个/秒 => rate/1e9 个/纳秒。桶容量封顶 rate(不攒过一秒的量)。
    if(nowNs > lastRefillNs_){
        double add = (double)(nowNs - lastRefillNs_) * (double)rate / 1e9;
        tokens_ += add;
        if(tokens_ > (double)rate){
            tokens_ = (double)rate;
        }
        lastRefillNs_ = nowNs;
    }
    if(tokens_ >= 1.0){
        tokens_ -= 1.0;
        return true;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}


BxLogger::BxLogger(const std::string& name):
    name_(name),
    level_(BxLogLevel::DEBUG){}

// 日志分发核心,够级别才输出。锁内只拷一份 appender 的 shared_ptr 快照
// (自己没挂就回退 root 的),出锁再做格式化和落盘。
// FATAL 特殊:先 flushAll 把异步队列和历史日志 drain 干净再同步写,保证崩溃前日志不丢。
void BxLogger::log(BxLogEvent::ptr event){
    // 够级别才输出
    if(event->getLevel() >= getLevel()){
        // 采样限流:ERROR/FATAL 永远全量(告警不能被采掉),低级别过令牌桶,超配额丢弃。
        // 在级别过滤之后、格式化/落盘之前判,丢弃的日志不产生任何 IO 开销。
        if(event->getLevel() < BxLogLevel::ERROR && !sampler_.allow()){
            return;
        }
        std::vector<BxLogAppender::ptr> appenders;
        BxLogger::ptr root;
        {
            // 只在锁内复制 shared_ptr 快照，不在锁内做格式化、入队或真实 IO。
            // 这样既避免 root fallback 遍历无锁导致的迭代器失效，也降低日志锁持有时间。
            MutexType::Lock lock(mutex_);
            if(!appenders_.empty()){
                appenders.assign(appenders_.begin(), appenders_.end());
            } else {
                root = root_;
            }
        }

        if(appenders.empty() && root){
            MutexType::Lock lock(root->mutex_);
            appenders.assign(root->appenders_.begin(), root->appenders_.end());
        }

        // FATAL 单点处理：先 drain 已进入异步队列/TLS 的历史日志，再同步落当前日志。
        // 并发线程在 flush 之后新产生的日志不属于这个同步屏障。
        if(event->getLevel() == BxLogLevel::FATAL){
            BxAsyncLoggerMgr::GetInstance()->flushAll();
        }

        for(auto& appender: appenders){
            if(appender){
                appender->log(event);
            }
        }
    }
}


// 给日志器追加一个输出地(持锁尾插)。配置热更新时,LogIniter 监听器通常先
// clearAppenders 再按 YAML 定义逐个 addAppender,实现 appender 的整体重建。
void BxLogger::addAppender(BxLogAppender::ptr appender){
    MutexType::Lock lock(mutex_);
    appenders_.push_back(appender);
}

void BxLogger::delAppender(BxLogAppender::ptr appender){
    MutexType::Lock lock(mutex_);
    for(auto it = appenders_.begin(); it != appenders_.end(); ++it){
        if(*it == appender){
            appenders_.erase(it);
            break;
        }
    }
}

void BxLogger::clearAppenders(){
    MutexType::Lock lock(mutex_);
    appenders_.clear();
}

void BxLogger::setLevel(BxLogLevel::Level level){
    level_.store(level, std::memory_order_release);
    inheritRootLevel_.store(false, std::memory_order_release);
}

std::string BxLogger::toYamlString() const {
    YAML::Node node(YAML::NodeType::Map);
    node["name"] = name_;
    node["level"] = BxLogLevel::ToString(getLevel());
    uint64_t sampleRate = getSampleRate();
    if(sampleRate != 0){
        node["sample_rate"] = sampleRate;
    }
    MutexType::Lock lock(mutex_);

    for(const auto& a : appenders_){
        node["appenders"].push_back(YAML::Load(a->toYamlString()));
    }
    std::stringstream ss;
    ss << node;
    return ss.str();
}


void BxLogAppender::setFormatter(BxLogFormatter::ptr formatter) {
    MutexType::Lock lock(mutex_);
    formatter_ = formatter;
}

BxLogFormatter::ptr BxLogAppender::getFormatter() const {
    MutexType::Lock lock(mutex_);
    return formatter_;
}

// appender 默认的 log:异步且非 FATAL 就 format 成 string 丢给 BxAsyncLogger,
// 否则(同步或 FATAL)直接 format 到 writeRaw。派生类只管写 writeRaw,异步这套在这统一管。
void BxLogAppender::log(BxLogEvent::ptr event){
    BxLogFormatter::ptr fmt;
    {
        MutexType::Lock lock(mutex_);
        fmt = formatter_ ? formatter_ : defaultFormatter_;
    }
    if(!fmt) return;

    std::ostringstream os;
    fmt->format(os, event);
    std::string formatted = os.str();
    if(isAsync() && event->getLevel() < BxLogLevel::FATAL){
        // 异步队列必须持有 shared_ptr，防止配置热更新清空 appender 后 writer 使用悬空指针。
        // 如果调用方用栈对象直接调用 log()，weak_from_this() 为空，退回同步写。
        auto self = weak_from_this().lock();
        if(self){
            BxAsyncLoggerMgr::GetInstance()->push(std::move(self), std::move(formatted));
        } else {
            writeRaw(formatted);
            flush();
        }
    } else {
        writeRaw(formatted);
        flush();
    }
}



BxStdoutLogAppender::BxStdoutLogAppender()
    : BxLogAppender(BxLogFormatter::ptr(new BxLogFormatter)){
};

void BxStdoutLogAppender::writeRaw(const std::string& s){
    // writer 线程或同步路径调用；std::cout 自带内部锁
    MutexType::Lock lock(mutex_);
    std::cout.write(s.data(), s.size());
}

void BxStdoutLogAppender::flush(){
    MutexType::Lock lock(mutex_);
    std::cout.flush();
}

std::string BxStdoutLogAppender::toYamlString() const {
    MutexType::Lock lock(mutex_);

    YAML::Node node(YAML::NodeType::Map);
    node["type"] = "BxStdoutLogAppender";
    // formatter不为默认值时输出其类型或 pattern
    if(formatter_){
        if(dynamic_cast<BxJsonFormatter*>(formatter_.get())){
            node["format"] = "json";
        } else {
            node["pattern"] = formatter_->getPattern();
        }
    }
    if(isAsync()){
        node["async"] = true;
    }
    std::stringstream ss;
    ss << node;
    return ss.str();
}

BxFileLogAppender::BxFileLogAppender(const std::string& filename)
    : BxLogAppender(BxLogFormatter::ptr(new BxLogFormatter))
    , filename_(filename){
}

void BxFileLogAppender::log(BxLogEvent::ptr event){
    BxLogAppender::log(event);
}

void BxFileLogAppender::writeRaw(const std::string& s){
    // writer 线程独占；同时同步路径下调用方已持有/或不需要 mutex_ 之外的同步
    MutexType::Lock lock(mutex_);
    // 定时 reopen 文件，防止文件被外部删除等
    uint64_t now = (uint64_t)time(nullptr);
    if(now >= (lastTime_ + 3)){
        if(!reopen()){
            std::cerr << "reopen fail, file name=" << filename_ << std::endl;
        }
        lastTime_ = now;
    }
    if(filestream_){
        filestream_.write(s.data(), s.size());
    }
}

void BxFileLogAppender::flush(){
    MutexType::Lock lock(mutex_);
    if(filestream_){
        filestream_.flush();
    }
}

std::string BxFileLogAppender::toYamlString() const {
    MutexType::Lock lock(mutex_);

    YAML::Node node(YAML::NodeType::Map);
    node["type"] = "BxFileLogAppender";
    node["file"] = filename_;
    // formatter不为默认值时输出其类型或 pattern
    if(formatter_){
        if(dynamic_cast<BxJsonFormatter*>(formatter_.get())){
            node["format"] = "json";
        } else {
            node["pattern"] = formatter_->getPattern();
        }
    }
    if(isAsync()){
        node["async"] = true;
    }
    std::stringstream ss;
    ss << node;
    return ss.str();
}

bool BxFileLogAppender::reopen()
{
    if(filestream_.is_open()){
        filestream_.close();
    }
    filestream_.clear();
    std::filesystem::path path(filename_);
    std::error_code ec;
    auto parent = path.parent_path();
    if(!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if(ec) return false;
    }
    filestream_.open(filename_, std::ios::app);
    return !!filestream_;
}


class MessageFormatItem: public BxLogFormatter::FormatItem{
public:
    MessageFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << event->getContent();
    }
};

class LevelFormatItem: public BxLogFormatter::FormatItem{
public:
    LevelFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << BxLogLevel::ToString(event->getLevel());
    }
};

class LoggerNameFormatItem: public BxLogFormatter::FormatItem{
public:
    LoggerNameFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << event->getLoggerName();
    }
};

class DateTimeFormatItem: public BxLogFormatter::FormatItem{
public:
    DateTimeFormatItem(const std::string& format = "%Y-%m-%d %H:%M:%S")
    : format_(format){
        if(format_.empty()){
            format_ = "%Y-%m-%d %H:%M:%S";
        }
    }

    void format(std::ostream& os, BxLogEvent::ptr event) override {    
        time_t time = event->getTime();
        struct tm tm;
        // 异步模式下格式化发生在业务线程，localtime() 的静态缓冲区会产生数据竞争。
        if(!localtime_r(&time, &tm)){
            return;
        }
        char buf[64];
        strftime(buf, sizeof(buf), format_.c_str(), &tm);
        os << buf;
    }

private:
    std::string format_;
};

class ElapseFormatItem: public BxLogFormatter::FormatItem{
public:
    ElapseFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << event->getElapse();
    }
};

class FilenameFormatItem: public BxLogFormatter::FormatItem{
public:
    FilenameFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << event->getFile();
    }
};

class LineFormatItem: public BxLogFormatter::FormatItem{
public:
    LineFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << event->getLine();
    }
};

class ThreadIdFormatItem: public BxLogFormatter::FormatItem{
public:
    ThreadIdFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << event->getThreadId();
    }
};

class FiberIdFormatItem: public BxLogFormatter::FormatItem{
public:
    FiberIdFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << event->getFiberId();
    }
};

class ThreadNameFormatItem: public BxLogFormatter::FormatItem{
public:
    ThreadNameFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << event->getThreadName();
    }
};

class PercentSignFormatItem: public BxLogFormatter::FormatItem{
public:
    PercentSignFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << "%";
    }
};

class TabFormatItem: public BxLogFormatter::FormatItem{
public:
    TabFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << '\t';
    }
};

class NewLineFormatItem: public BxLogFormatter::FormatItem{
public:
    NewLineFormatItem(const std::string& str = ""){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << std::endl;
    }
};

class StringFormatItem: public BxLogFormatter::FormatItem{
public:
    StringFormatItem(const std::string& str = ""): string_(str){}
    void format(std::ostream& os, BxLogEvent::ptr event) override {
        os << string_;
    }

private:
    std::string string_;
};


// 初始化LogFormatter，默认pattern为"%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n" 
BxLogFormatter::BxLogFormatter(const std::string& pattern):
    pattern_(pattern),
    error_(false){
        init();
    }


std::ostream& BxLogFormatter::format(std::ostream& os, BxLogEvent::ptr event){
    for(auto& formatItem : items_){
        formatItem->format(os, event);
    }
    return os;
}


// JSON 字符串转义:引号/反斜杠/控制字符按 RFC 8259 转成 \" \\ \n \t \uXXXX 等。
// 直接写 os,不构造中间 string,零额外分配。
static void JsonEscape(std::ostream& os, const std::string& s){
    for(unsigned char c : s){
        switch(c){
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            default:
                if(c < 0x20){
                    // 其余控制字符走 \u00XX
                    static const char* hex = "0123456789abcdef";
                    os << "\\u00" << hex[(c >> 4) & 0xF] << hex[c & 0xF];
                } else {
                    os << (char)c;
                }
        }
    }
}

std::ostream& BxJsonFormatter::format(std::ostream& os, BxLogEvent::ptr event){
    // 时间格式化成 ISO-ish 字符串(和 pattern 的 %d 默认一致,便于下游统一解析)。
    time_t t = (time_t)event->getTime();
    struct tm tm;
    char timebuf[64] = {0};
    if(localtime_r(&t, &tm)){
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
    }

    os << "{\"time\":\"" << timebuf << "\",";
    os << "\"level\":\"" << BxLogLevel::ToString(event->getLevel()) << "\",";
    os << "\"logger\":\"";      JsonEscape(os, event->getLoggerName()); os << "\",";
    os << "\"thread\":" << event->getThreadId() << ",";
    os << "\"thread_name\":\""; JsonEscape(os, event->getThreadName()); os << "\",";
    os << "\"fiber\":" << event->getFiberId() << ",";
    os << "\"file\":\"";        JsonEscape(os, event->getFile()); os << "\",";
    os << "\"line\":" << event->getLine() << ",";
    os << "\"msg\":\"";         JsonEscape(os, event->getContent()); os << "\"";
    os << "}\n";
    return os;
}


void BxLogFormatter::init(){
    // <type, str>
    // type: 为0时表示为普通字符串，为1时表示为需要解析的模板参数
    // str: 存储字符串内容（普通字符串 or 模板参数）
    std::vector<std::pair<int, std::string>> vec;

    // 模板参数中只有%d（日期时间）需要额外存储模板字符串，所以使用一个单独的变量dataFormat来存储
    std::string dateFormat;
    // 存储常规字符串
    std::string normalString;
    // 状态机，true表示正在解析普通字符串，false表示正在解析模板参数或模板字符串
    bool parsing_string = true;

    for(size_t i = 0; i < pattern_.size(); ++i){
        std::string c = std::string(1, pattern_[i]);
        if(c == "%"){
            if(parsing_string){
                // 解析常规字符串时碰到%，本段常规字符串的解析完成，状态机变为解析模板字符串模式
                if(!normalString.empty()){
                    vec.push_back(std::make_pair(0, normalString));
                    normalString.clear();
                }
                parsing_string = false;
                continue;
            } else {
                // 解析模板参数时碰到%，说明是转义%
                vec.push_back(std::make_pair(1, c));
                parsing_string = true;                
            }
        } else {
            if(parsing_string){
                // 解析常规字符串时，不断将当前字符加入normalString，直到遇到%
                normalString += c;
                continue;
            } else {
                // 解析模板字符：直接将模板字符加入vec
                vec.push_back(std::make_pair(1, c));   

                // 对日期时间（%d），还需要另外解析dateFormat
                if(c == "d"){
                    ++i;
                    if(i >= pattern_.size()){
                        // "%d" at end is valid; DateTimeFormatItem will use its default pattern.
                        parsing_string = true;
                        break;
                    }
                    if(pattern_[i] != '{'){
                        // %d后没有跟 {}，dateFormat为空，解析结果依赖DataFormatItem类的默认实现
                        --i;
                        parsing_string = true;
                        continue;
                    } else {
                        ++i;
                        // 将 {} 之间的所有字符都加入dateFormat
                        while(i < pattern_.size() && pattern_[i] != '}'){
                            dateFormat.push_back(pattern_[i]);
                            ++i;
                        }
                        // 如果遍历到m_pattern尾部都没有遇到'}'，说明大括号没有闭合，解析错误
                        if(i == pattern_.size() && pattern_[i - 1] != '}'){
                            // log中加入错误信息
                            vec.push_back(std::make_pair(0, "<<Pattern Error>>"));
                            dateFormat.clear();
                            error_ = true;
                        }
                    }
                }
                // 模板字符串解析完成，状态机转为默认状态（即解析常规字符串）
                parsing_string = true;
            }
        }
    }
    // 将尾部的常规字符串也加入vec
    if(!normalString.empty()){
        vec.push_back(std::pair(0, normalString));
        normalString.clear();
    }
    if(!parsing_string){
        vec.push_back(std::make_pair(0, "<<Pattern Error>>"));
        error_ = true;
    }

    // pattern 字符 -> 对应 FormatItem 的映射表,用 XX 宏挨个填
    static std::map<std::string, std::function<FormatItem::ptr(const std::string& str)>> s_format_items = {
#define XX(str, C) {#str, [](const std::string& fmt){ return FormatItem::ptr(new C(fmt)); } }

        XX(m, MessageFormatItem),       // m: 消息
        XX(p, LevelFormatItem),         // p: 日志级别
        XX(c, LoggerNameFormatItem),    // c: 日志器名称
        XX(d, DateTimeFormatItem),      // d: 日期时间
        XX(r, ElapseFormatItem),        // r: 累计毫秒数
        XX(f, FilenameFormatItem),      // f: 文件名
        XX(l, LineFormatItem),          // l: 行号
        XX(t, ThreadIdFormatItem),      // t: 线程号
        XX(F, FiberIdFormatItem),       // F: 协程号
        XX(N, ThreadNameFormatItem),    // N: 线程名称
        XX(%, PercentSignFormatItem),   // %: 百分号
        XX(T, TabFormatItem),           // T: 制表符
        XX(n, NewLineFormatItem),       // n: 换行符

#undef XX
    };

    for(auto& item : vec){
        // 常规字符串
        if(item.first == 0){
            items_.push_back(FormatItem::ptr(new StringFormatItem(item.second)));
        }
        // 模板参数
        else {
            // 日期时间进行特殊处理，使用dateFormat初始化，其他模板参数使用item.second初始化
            if(item.second == "d"){
                items_.push_back(FormatItem::ptr(new DateTimeFormatItem(dateFormat)));
            } else {
                auto it = s_format_items.find(item.second);
                if(it != s_format_items.end()){
                    items_.push_back(FormatItem::ptr(it->second(item.second)));
                } else {
                    // 不合法的模板参数，log记录错误信息
                    items_.push_back(FormatItem::ptr(new StringFormatItem("<<error_format %" + item.second + ">>")));
                }
            }
        }

        // for debug
        // std::cout << "FormatItem type: " << item.first << ", \tContent: " << item.second << std::endl;
        // if(item.first == 1 && item.second == "d"){
        //     std::cout << "Date Format: " << dateFormat << std::endl;
        // }
    }
    // std::cout << "BxLogFormatter init complete!" << std::endl;
}


BxLogEventWrap::BxLogEventWrap(BxLogger::ptr logger, BxLogEvent::ptr event)
    : logger_(logger)
    , event_(event){
}

// LogEventWrap析构时对日志事件进行log
BxLogEventWrap::~BxLogEventWrap(){
    logger_->log(event_);
}

std::stringstream& BxLogEventWrap::getSS(){
    return event_->getSS();
}


BxLogManager::BxLogManager(){
    root_.reset(new BxLogger("root"));
    root_->addAppender(std::make_shared<BxStdoutLogAppender>());
    loggers_[root_->getName()] = root_;
    init();
}

// 取一个具名日志器,没有就现建。宏 BRONX_LOG_NAME 落到这。
// 新建的 logger 的 root_ 指向本管理器的 root,所以没配项时 level 和输出都会回退 root。
BxLogger::ptr BxLogManager::getLogger(std::string loggerName){
    MutexType::Lock lock(mutex_);

    auto it = loggers_.find(loggerName);
    // 找到logger就返回该logger
    if(it != loggers_.end()){
        return it->second;
    }
    // 否则以该名字创建一个logger，其主日志器指向LogManager的主日志器
    BxLogger::ptr newLogger(std::make_shared<BxLogger>(loggerName));
    newLogger->root_ = root_;
    newLogger->inheritRootLevel_.store(true, std::memory_order_release);
    loggers_[loggerName] = newLogger;
    return newLogger;
}

std::string BxLogManager::toYamlString() const {
    MutexType::Lock lock(mutex_);
    YAML::Node node(YAML::NodeType::Map);
    for(const auto& l : loggers_){
        node["logs"].push_back(YAML::Load(l.second->toYamlString()));
    }
    std::stringstream ss;
    ss << node;
    return ss.str();
}

// ----------------------------------------
// | 与配置系统整合，从配置文件中获取配置信息 |
// ----------------------------------------

// 记录appender信息的结构体
struct LogAppenderDefine{
    int type = 0;   // 1: File, 2: Stdout
    std::string pattern;
    std::string file;
    bool async = false;
    bool json = false;   // true: 用 BxJsonFormatter 结构化输出(与 pattern 互斥,json 优先)

    bool operator==(const LogAppenderDefine& oth) const {
        return type == oth.type
            && pattern == oth.pattern
            && file == oth.file
            && async == oth.async
            && json == oth.json;
    }
};

// 记录Logger相关信息的结构体
struct LoggerDefine{
    std::string name;
    BxLogLevel::Level level = BxLogLevel::UNKNOW;
    uint64_t sampleRate = 0;   // 每秒采样上限,0=不限流(ERROR/FATAL 恒豁免)
    std::vector<LogAppenderDefine> appenders;

    bool operator==(const LoggerDefine& oth) const {
        return name == oth.name
            && level == oth.level
            && sampleRate == oth.sampleRate
            && appenders == oth.appenders;
    }

    bool operator!=(const LoggerDefine& oth) const {
        return !(*this == oth);
    }

    bool operator<(const LoggerDefine& oth) const {
        return name < oth.name;
    }
};


// 偏特化Lexical_cast
// Yaml字符串 -> LoggerDefine
template<>
class LexicalCast<std::string, LoggerDefine>{
public:
    LoggerDefine operator()(const std::string& val){
        YAML::Node node = YAML::Load(val);
        LoggerDefine ld;
        if(!node["name"].IsDefined()){
            std::cerr << "log config error: name is null" << node << std::endl;
            throw std::logic_error("log config name is null");
        }
        // 处理name和level
        ld.name = node["name"].as<std::string>();
        ld.level = BxLogLevel::FromString(node["level"].IsDefined() ? node["level"].as<std::string>() : "");
        // 采样限流:sample_rate 条/秒,缺省 0(不限流)
        if(node["sample_rate"].IsDefined()){
            ld.sampleRate = node["sample_rate"].as<uint64_t>();
        }
        // 处理appenders
        if(node["appenders"].IsDefined()){
            for(auto a : node["appenders"]){
                if(!a["type"].IsDefined()){
                    std::cerr << "log config error: appender type is null" << a << std::endl;
                    continue;
                }
                LogAppenderDefine lad;

                // 不同的appender类型设置不同的type，FileLogAppender还需要获取输出的文件路径
                std::string type = a["type"].as<std::string>();
                if(type == "BxFileLogAppender"){
                    lad.type = 1;
                    if(!a["file"].IsDefined()){
                        std::cerr << "log config error: BxFileLogAppender filename is null" << a << std::endl;
                        continue;
                    }
                    lad.file = a["file"].as<std::string>();
                } else if(type == "BxStdoutLogAppender"){
                    lad.type = 2;
                } else {
                    std::cerr << "log config error: appender type is invalid" << a << std::endl;
                    continue;
                }
                // 以上appender都需要检测是否配置了formatter的pattern
                if(a["pattern"].IsDefined()){
                    lad.pattern = a["pattern"].as<std::string>();
                }
                // 异步开关
                if(a["async"].IsDefined()){
                    lad.async = a["async"].as<bool>();
                }
                // 结构化输出:format: json => BxJsonFormatter(与 pattern 互斥)
                if(a["format"].IsDefined() && a["format"].as<std::string>() == "json"){
                    lad.json = true;
                }
                // 将处理完成的LogAppenderDefine加入appenders中
                ld.appenders.push_back(lad);
            }
        }
        return ld;
    }
};

// LoggerDefine -> Yaml字符串
template<>
class LexicalCast<LoggerDefine, std::string>{
public:
    std::string operator()(const LoggerDefine& ld){
        YAML::Node node(YAML::NodeType::Map);
        node["name"] = ld.name;
        node["level"] = BxLogLevel::ToString(ld.level);
        if(ld.sampleRate != 0){
            node["sample_rate"] = ld.sampleRate;
        }
        std::stringstream ss_appenders;
        for(const auto& lad : ld.appenders){
            YAML::Node nlad(YAML::NodeType::Map);
            if(lad.type == 1){
                nlad["type"] = "BxFileLogAppender";
                nlad["file"] = lad.file;
            } else if (lad.type == 2){
                nlad["type"] = "BxStdoutLogAppender";
            }
            if(!lad.pattern.empty()){
                nlad["pattern"] = lad.pattern;
            }
            if(lad.async){
                nlad["async"] = true;
            }
            if(lad.json){
                nlad["format"] = "json";
            }
            node["appenders"].push_back(nlad);
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};


// 注册 "logs" 配置项:把日志配置抽象成一组 LoggerDefine,以 BxConfigVar 形式登记到
// BxConfig 全局表。LogIniter 给它挂监听器,LoadFromYaml 灌值时即触发日志器重建。
BxConfigVar<std::set<LoggerDefine>>::ptr g_log_config =
        BxConfig::Lookup("logs", std::set<LoggerDefine>(), "logs config");


// 把"配置变了"接到"重建日志器"的那座桥,是日志层接入热更新的关键。
// 全局对象 __log_init 在 main 之前构造,构造时给 g_log_config("logs" 项)挂个监听器。
// 之后 LoadFromYaml("logs") 一改值就触发:比对新旧 LoggerDefine 集合,
// 新增/改动的用 BRONX_LOG_NAME 取出 logger 重设 level 和 appender,删掉的把级别设成 NOTSET 清空 appender。
struct LogIniter{
    LogIniter(){
        // BRONX_LOG_INFO(BRONX_LOG_ROOT()) << "g_log_config add callback";
        g_log_config->addListener([](const std::set<LoggerDefine>& oldVal,
                                     const std::set<LoggerDefine>& newVal){
            // 监测以下三种情况：新增 / 修改 / 删除
            for(auto& i : newVal){
                auto it = oldVal.find(i);
                BxLogger::ptr logger;
                if(it == oldVal.end()){
                    // <新增>
                    logger = BRONX_LOG_NAME(i.name);
                } else {
                    if(i != *it){
                        // <修改>
                        logger = BRONX_LOG_NAME(i.name);
                    } else {
                        continue;
                    }
                }
                // 根据newVal中的数据成员修改logger配置
                // 设置level
                logger->setLevel(i.level);
                // 设置采样限流(热更新可动态调整每秒配额,0=关闭)
                logger->setSampleRate(i.sampleRate);
                // 设置appenders
                logger->clearAppenders();
                for(auto &a : i.appenders){
                    BxLogAppender::ptr ap;
                    if(a.type == 1){
                        ap.reset(new BxFileLogAppender(a.file));
                    } else if(a.type == 2){
                        ap.reset(new BxStdoutLogAppender());
                    }
                    // format: json 优先(结构化输出);否则 pattern 非空用自定义 pattern;都无则默认
                    if(a.json){
                        ap->setFormatter(BxLogFormatter::ptr(new BxJsonFormatter()));
                    } else if(!a.pattern.empty()){
                        ap->setFormatter(BxLogFormatter::ptr(new BxLogFormatter(a.pattern)));
                    }
                    if(ap){
                        ap->setAsync(a.async);
                    }
                    logger->addAppender(ap);
                }
            }

            // <删除>
            for(auto& i : oldVal){
                auto it = newVal.find(i);
                if(it == newVal.end()){
                    // 对配置文件中被删除的logger执行逻辑删除
                    BxLogger::ptr logger = BRONX_LOG_NAME(i.name);
                    // 逻辑删除：将Level设置为访问不到的级别，并清空所有appender
                    logger->setLevel(BxLogLevel::NOTSET);
                    logger->clearAppenders();
                }
            }
        });
    }
};
static LogIniter __log_init;


// 从配置文件中加载日志配置
void BxLogManager::init(){
    
}

}
