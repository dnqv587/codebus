#pragma once
#include <sys/types.h>

/// @brief 隐式类型转换
template<typename To,typename From>
inline To implicit_cast(const From& f)
{return f;}

/// @brief 文件大小字面量
constexpr off_t operator""_KB(unsigned long long int size)
{
    return size*1024;
}

constexpr off_t operator""_MB(unsigned long long int size)
{
    return size*1024*1024;
}

constexpr off64_t operator""_GB(unsigned long long int size)
{
    return size*1024*1024*1024;
}

constexpr off64_t operator""_TB(unsigned long long int size)
{
    return size*1024*1024*1024*1024;
}


