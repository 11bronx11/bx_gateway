#include "util.h"
#include "log.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sstream>
#include <ctime>


static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

namespace bronx{


pid_t GetThreadId(){
    // return pthread_self();
    // syscall(SYS_gettid)返回一个系统范围内唯一的线程id
    return syscall(SYS_gettid);
}


std::string GetThreadName(){
    return BxThread::GetName();
}



uint64_t CurrentId(){
    return bronx::BxFiber::CurrentId();
}

// 获取当前调用栈信息
void Backtrace(std::vector<std::string> &bt, int size, int skip){
    void** buffer = (void**)malloc(sizeof(void*) * size);
    // 获取函数调用栈地址
    size_t s = ::backtrace(buffer, size);
    // 将函数栈地址转换为描述函数调用信息的字符串
    char** strings = backtrace_symbols(buffer, s);  
    if(strings == NULL){
        BRONX_LOG_ERROR(g_logger) << "backtrace_symbols error";
        free(buffer);
        free(strings);
        return;
    }

    for(size_t i = skip; i < s; ++i){
        bt.push_back(strings[i]);
    }

    free(buffer);
    free(strings);
};

// 获取当前调用栈信息的字符串
std::string BacktraceToString(int size, const std::string& prefix, int skip){
    std::vector<std::string> bt;
    Backtrace(bt, size, skip);
    std::stringstream ss;
    for(auto& s : bt){
        ss << prefix << s << std::endl;
    }
    return ss.str();
};


// 获取当前时间（毫秒）
uint64_t GetCurrentMs(){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000ul + tv.tv_usec / 1000;
}

// 获取当前时间（微秒）
uint64_t GetCurrentUs(){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 * 1000ul + tv.tv_usec;
}

std::string GetCurrentTimeString(){
    time_t now = time(nullptr);
    struct tm t;
    if(!localtime_r(&now, &t)){
        return "";
    }

    char buffer[32];
    if(strftime(buffer, sizeof(buffer), "%Y.%m.%d %H:%M", &t) == 0){
        return "";
    }
    
    return std::string(buffer);
}


int8_t  TypeUtil::ToChar(const std::string& str) {
    if(str.empty()) {
        return 0;
    }
    return *str.begin();
}

int64_t TypeUtil::Atoi(const std::string& str) {
    if(str.empty()) {
        return 0;
    }
    return strtoull(str.c_str(), nullptr, 10);
}

double  TypeUtil::Atof(const std::string& str) {
    if(str.empty()) {
        return 0;
    }
    return atof(str.c_str());
}

int8_t  TypeUtil::ToChar(const char* str) {
    if(str == nullptr) {
        return 0;
    }
    return str[0];
}

int64_t TypeUtil::Atoi(const char* str) {
    if(str == nullptr) {
        return 0;
    }
    return strtoull(str, nullptr, 10);
}

double  TypeUtil::Atof(const char* str) {
    if(str == nullptr) {
        return 0;
    }
    return atof(str);
}


}
