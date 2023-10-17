#pragma once
#include <base/noncopyable.hpp>
#include <thread/Mutex.hpp>
#include <time/Timestamp.h>
#include <pthread.h>

class Condition:noncopyable
{
public:
    explicit Condition(MutexLock& mutex)
    :m_mutex(mutex)
    {
        ::pthread_cond_init(&m_cond, nullptr);
    }

    ~Condition()
    {
        ::pthread_cond_destroy(&m_cond);
    }

    void wait()
    {
        ::pthread_cond_wait(&m_cond,m_mutex.getNaitiveHandle());
    }

    /// @brief 相对时间内等待条件满足[s]
    /// @param seconds
    /// @return 超时返回false,条件满足返回true
    bool TimeWait(int seconds)
    {
        struct timespec abstime;
        clock_gettime(CLOCK_REALTIME, &abstime);
        abstime.tv_sec += seconds;
        return ETIMEDOUT == ::pthread_cond_timedwait(&m_cond, m_mutex.getNaitiveHandle(), &abstime);
    }

    /// @brief  通知一个阻塞在当前条件变量的线程
    void notify()
    {
        ::pthread_cond_signal(&m_cond);
    }

    /// @brief 通知全部阻塞在当前条件变量的线程
    void notifyAll()
    {
        ::pthread_cond_broadcast(&m_cond);
    }


private:
    MutexLock& m_mutex;
    pthread_cond_t m_cond;
};