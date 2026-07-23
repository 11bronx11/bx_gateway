#pragma once

#define BRONX_LITTLE_ENDIAN 1
#define BRONX_BIG_ENDIAN 2

#include <byteswap.h>
#include <stdint.h>
#include <iostream>
#include "macro.h"

namespace bronx{

// 8字节类型的字节序转化
template<class T>
typename std::enable_if<sizeof(T) == sizeof(uint64_t), T>::type
byteswap(T value) {
    // -字节序反转
    return (T)bswap_64((uint64_t)value);
}

// 4字节类型的字节序转化
template<class T>
typename std::enable_if<sizeof(T) == sizeof(uint32_t), T>::type
byteswap(T value) {
    return (T)bswap_32((uint32_t)value);
}

// 2字节类型的字节序转化
template<class T>
typename std::enable_if<sizeof(T) == sizeof(uint16_t), T>::type
byteswap(T value) {
    return (T)bswap_16((uint16_t)value);
}


#if BYTE_ORDER == BIG_ENDIAN
#define BRONX_BYTE_ORDER BRONX_BIG_ENDIAN
#else
#define BRONX_BYTE_ORDER BRONX_LITTLE_ENDIAN
#endif

#if BRONX_BYTE_ORDER == BRONX_BIG_ENDIAN

// 设备为大端字节序时：
template<typename T>
T byteswapToBigEndian(T t){
    return t;
}

template<typename T>
T byteswapToLittleEndian(T t){
    return byteswap(t);
}

#else

// 设备为小端字节序时：
template<typename T>
T byteswapToBigEndian(T t){
    return byteswap(t);
}

template<typename T>
T byteswapToLittleEndian(T t){
    return t;
}

#endif

}
