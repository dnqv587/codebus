#pragma once
#include <base/noncopyable.hpp>
#include <base/Macro.h>
#include <pthread.h>

/// @brief 互斥锁
class MutexLock: noncopyable
{
public:
    MutexLock() noexcept
    {::pthread_mutex_init(&m_mutex,NULL);}
    ~MutexLock() noexcept
    {::pthread_mutex_destroy(&m_mutex);}
    /// @brief 加锁
    void lock() noexcept
    {::pthread_mutex_lock(&m_mutex);}
    /// @brief 解锁
    void unlock() noexcept
    {::pthread_mutex_unlock(&m_mutex);}
    /// @brief 获得锁指针
    /// @return
    pthread_mutex_t* getNaitiveHandle()
    {return &m_mutex;}
private:
    pthread_mutex_t m_mutex;
};

/// @brief 使用RAII进行进行加锁
class MutexLockGuard:noncopyable
{
public:
    MutexLockGuard(MutexLock& mutex):m_MutexLock(mutex)
    {m_MutexLock.lock();}
    ~MutexLockGuard()
    {m_MutexLock.unlock();}
private:
    MutexLock& m_MutexLock;
};

/// @brief 防止未定义变量名，导致产生临时对象
#define MutexLockGuard(x)  static_assert(false,"missing MutexLockGuard var name")