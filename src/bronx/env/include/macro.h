#pragma once

#include <assert.h>
#include "log.h"
#include "util.h"


// LIKELY宏封装
#if defined __GNUC__ || defined __llvm__
// 告诉编译器优化,条件大概率成立
#define BRONX_LIKELY(x) __builtin_expect(!!(x), 1)
// 告诉编译器优化,条件大概率不成立
#define BRONX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define BRONX_LIKELY(x) (x)
#define BRONX_UNLIKELY(x) (x)
#endif



// 断言宏封装
// log记录函数调用栈信息
#define BRONX_ASSERT(x) \
    if(BRONX_UNLIKELY(!(x))){ \
        BRONX_LOG_ERROR(BRONX_LOG_ROOT()) << "ASSERTION: " #x \
            << "\nbacktrace:\n" \
            << bronx::BacktraceToString(100, "    "); \
        assert(x); \
    }

// 断言宏封装，可多传入一个参数w
#define BRONX_ASSERT2(x, w) \
    if(BRONX_UNLIKELY(!(x))){ \
        BRONX_LOG_ERROR(BRONX_LOG_ROOT()) << "ASSERTION: " #x \
            << "\n" << w \
            << "\nbacktrace:\n" \
            << bronx::BacktraceToString(100, "    "); \
        assert(x); \
    }

