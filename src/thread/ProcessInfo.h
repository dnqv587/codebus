#include <unistd.h>
#include <string>

namespace ProcessInfo
{
/// @brief 获取进程ID
/// @return Pid
inline pid_t Pid() noexcept
{
    return ::getpid();
}

/// @brief 获取主机名
/// @return hostname
std::string hostname();

}
