# pragma once

#include <vector>
#include <string>
#include <pthread.h>
#include <execinfo.h>
#include "thread.h"
#include "fiber.h"


namespace bronx{

// 返回内核态线程 id(syscall SYS_gettid,全系统唯一,区别于 pthread_self 的用户态 id)
pid_t GetThreadId();

std::string GetThreadName();

uint64_t CurrentId();


// 获取当前调用栈信息
// bt：保存调用栈的容器，传出参数
// size：记录的最大调用栈层数
// skip：跳过的调用栈层数
void Backtrace(std::vector<std::string> &bt, int size = 64, int skip = 1);

// 获取当前调用栈信息的字符串
std::string BacktraceToString(int size = 64, const std::string& prefix = "", int skip = 2);

// 获取当前时间（毫秒）
uint64_t GetCurrentMs();

// 获取当前时间（微秒）
uint64_t GetCurrentUs();

// 返回当前时间的格式化字符串（北京时间）
std::string GetCurrentTimeString();

class TypeUtil {
    public:
        static int8_t ToChar(const std::string& str);
        static int64_t Atoi(const std::string& str);
        static double Atof(const std::string& str);
        static int8_t ToChar(const char* str);
        static int64_t Atoi(const char* str);
        static double Atof(const char* str);
};



}