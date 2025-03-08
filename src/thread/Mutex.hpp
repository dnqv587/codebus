#pragma once
#include <base/noncopyable.hpp>
#include <pthread.h>
#include "base/Macro.h"

namespace bus
{
/// @brief 互斥锁
class MutexLock : noncopyable
{
 public:
	ATTR_INLINE
	MutexLock() noexcept
		: m_mutex()
	{
		::pthread_mutex_init(&m_mutex, nullptr);
	}
	ATTR_INLINE
	~MutexLock() noexcept
	{
		::pthread_mutex_destroy(&m_mutex);
	}
	/// @brief 加锁
	ATTR_INLINE
	void lock() noexcept
	{
		::pthread_mutex_lock(&m_mutex);
	}
	/// @brief 解锁
	ATTR_INLINE
	void unlock() noexcept
	{
		::pthread_mutex_unlock(&m_mutex);
	}
	/// @brief 获得锁指针
	/// @return
	ATTR_INLINE
	pthread_mutex_t* getNativeHandle()
	{
		return &m_mutex;
	}
 private:
	pthread_mutex_t m_mutex;
};

/// @brief 使用RAII进行加锁
class MutexLockGuard : noncopyable
{
 public:
	ATTR_INLINE
	explicit MutexLockGuard(MutexLock& mutex) : m_MutexLock(mutex)
	{
		m_MutexLock.lock();
	}
	ATTR_INLINE
	~MutexLockGuard()
	{
		m_MutexLock.unlock();
	}
 private:
	MutexLock& m_MutexLock;
};

/// @brief 读写锁
class RWLock : noncopyable
{
 public:
	ATTR_INLINE
	RWLock() noexcept
		: m_rwlock{}
	{
		::pthread_rwlock_init(&m_rwlock, nullptr);
	}
	ATTR_INLINE
	~RWLock() noexcept
	{
		::pthread_rwlock_destroy(&m_rwlock);
	}

	/// @brief 读锁
	ATTR_INLINE
	void rdlock() noexcept
	{
		::pthread_rwlock_rdlock(&m_rwlock);
	}

	/// @brief 写锁
	ATTR_INLINE
	void wrlock() noexcept
	{
		::pthread_rwlock_wrlock(&m_rwlock);
	}

	/// @brief 解锁
	ATTR_INLINE
	void unlock() noexcept
	{
		::pthread_rwlock_unlock(&m_rwlock);
	}

 private:
	pthread_rwlock_t m_rwlock;
};

class ReadLockGuard : noncopyable
{
 public:
	ATTR_INLINE
	explicit ReadLockGuard(RWLock& rwlock) noexcept
		: m_rwlock(rwlock)
	{
		m_rwlock.rdlock();
	}

	ATTR_INLINE
	~ReadLockGuard() noexcept
	{
		m_rwlock.unlock();
	}
 private:
	RWLock& m_rwlock;
};

class WriteLockGuard : noncopyable
{
 public:
	ATTR_INLINE
	explicit WriteLockGuard(RWLock& rwlock) noexcept
		: m_rwlock(rwlock)
	{
		m_rwlock.wrlock();
	}

	ATTR_INLINE
	~WriteLockGuard() noexcept
	{
		m_rwlock.unlock();
	}
 private:
	RWLock& m_rwlock;
};

/// @brief 防止未定义变量名，导致产生临时对象
#define MutexLockGuard(x)  static_assert(false,"missing MutexLockGuard var name")
#define ReadLockGuard(x)  static_assert(false,"missing ReadLockGuard var name")
#define WriteLockGuard(x)  static_assert(false,"missing WriteLockGuard var name")

}