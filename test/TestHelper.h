//***********************************************
// name      : TestHelper.h
// brief     : 单元测试公共辅助设施（临时目录、日志捕获、fd 统计、空闲端口探测）
// attention : 仅供测试代码使用
//***********************************************
#pragma once
#include "log/LogAppender.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/// @brief 是否运行在 sanitizer（TSAN/ASAN）构建之下
/// @attention 死亡测试（EXPECT_DEATH/abort 类用例）在 sanitizer 下不稳定，
///             用例内以 `if (CODEBUS_TEST_UNDER_SANITIZER) GTEST_SKIP();` 守卫
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#    define CODEBUS_TEST_UNDER_SANITIZER 1
#  else
#    define CODEBUS_TEST_UNDER_SANITIZER 0
#  endif
#elif defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#  define CODEBUS_TEST_UNDER_SANITIZER 1
#else
#  define CODEBUS_TEST_UNDER_SANITIZER 0
#endif

/// @brief 是否运行在 ThreadSanitizer 构建之下
/// @attention TSAN 不支持多线程 fork 后再创建线程（die_after_fork），
///             子进程内新建线程的死亡测试须以本宏跳过
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CODEBUS_TEST_UNDER_TSAN 1
#  else
#    define CODEBUS_TEST_UNDER_TSAN 0
#  endif
#elif defined(__SANITIZE_THREAD__)
#  define CODEBUS_TEST_UNDER_TSAN 1
#else
#  define CODEBUS_TEST_UNDER_TSAN 0
#endif

namespace bus
{
namespace test
{

/// @brief 临时目录 RAII，析构时递归删除
class TempDir
{
 public:
    TempDir()
    {
        static std::atomic<uint64_t> counter{0};
        m_path = std::filesystem::temp_directory_path() /
                 ("codebus_test_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::create_directories(m_path, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// @brief 获取临时目录路径
    ATTR_PURE_INLINE
    const std::filesystem::path& Path() const noexcept
    {
        return m_path;
    }

 private:
    /// @brief 临时目录路径
    std::filesystem::path m_path;
};

/// @brief 捕获日志到内存的 Appender，用于断言日志内容
class CaptureAppender : public LogAppender
{
 public:
    void append(std::string_view logStream) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_content.append(logStream.data(), logStream.size());
    }

    void append(std::string_view logStream, LogLevel) override
    {
        append(logStream);
    }

    /// @brief 获取已捕获内容
    std::string Contents() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_content;
    }

    /// @brief 清空已捕获内容
    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_content.clear();
    }

 private:
    /// @brief 保护捕获内容
    mutable std::mutex m_mutex;
    /// @brief 已捕获日志
    std::string m_content;
};

/// @brief 统计当前进程打开的 fd 数量
/// @return /proc/self/fd 目录项数量
inline int FdCount()
{
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr)
    {
        return -1;
    }
    int count = 0;
    while (::readdir(dir) != nullptr)
    {
        ++count;
    }
    ::closedir(dir);
    // 排除 "." 与 ".."
    return count - 2;
}

/// @brief 探测一个空闲的本地 TCP 端口
/// @return 端口号（1-65534）
inline uint16_t GetFreePort()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0)
    {
        ::close(fd);
        return 0;
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}

/// @brief fd RAII
class ScopedFd
{
 public:
    explicit ScopedFd(int fd = -1) noexcept : m_fd(fd) {}

    ~ScopedFd()
    {
        reset();
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : m_fd(other.release()) {}

    ScopedFd& operator=(ScopedFd&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }

    ATTR_PURE_INLINE
    int get() const noexcept
    {
        return m_fd;
    }

    ATTR_PURE_INLINE
    bool valid() const noexcept
    {
        return m_fd >= 0;
    }

    int release() noexcept
    {
        int fd = m_fd;
        m_fd = -1;
        return fd;
    }

    void reset(int fd = -1) noexcept
    {
        if (m_fd >= 0)
        {
            ::close(m_fd);
        }
        m_fd = fd;
    }

 private:
    /// @brief 管理的 fd
    int m_fd;
};

/// @brief 在超时时间内等待条件成立
/// @param pred 条件谓词
/// @param timeout 超时时间
/// @return 条件成立返回 true，超时返回 false
/// @attention 仅用于测试中的短等待；优先使用 CountDownLatch/promise
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace test
} // namespace bus
