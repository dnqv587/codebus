#pragma once
#include <sys/types.h>
#include <base/noncopyable.hpp>


class CurrentThread :noncopyable
{
public:
    /// @brief 线程号
    /// @return
    static pid_t tid();
    /// @brief 线程名
    /// @return
    static const char* ThreadName();
    /// @brief 是否为主线程
    /// @return
    static bool isMainThread();

protected:
    static void setThreadName(const char* threadName);
    static void setThreadId(pid_t threadId);

private:
	thread_local static const char* t_threadName;
	thread_local static pid_t t_threadId;
};
