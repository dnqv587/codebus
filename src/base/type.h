#pragma once
#include <sys/types.h>
#include <cstdint>
#include <type_traits>

namespace bus
{

/// @brief socket类型
typedef int socket_t;

typedef uint32_t event_t;

/// @brief 隐式类型转换
template<typename To, typename From>
inline To implicit_cast(const From &f)
{ return f; }

template<typename T,typename... U>
inline constexpr bool is_one_of = ( ... || std::is_same_v<T,U>);


/// @brief 文件大小字面量
constexpr off_t operator ""_KB(unsigned long long int size)
{
    return size * 1024;
}

constexpr off_t operator ""_MB(unsigned long long int size)
{
    return size * 1024 * 1024;
}

constexpr off64_t operator ""_GB(unsigned long long int size)
{
    return size * 1024ll * 1024ll * 1024ll;
}

constexpr off64_t operator ""_TB(unsigned long long int size)
{
    return size * 1024ll * 1024ll * 1024ll * 1024ll;
}


}
