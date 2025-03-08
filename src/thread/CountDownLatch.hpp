#pragma once
#include <base/noncopyable.hpp>
#include <thread/Mutex.hpp>
#include <thread/Condition.hpp>

namespace bus
{
//***********************************************
// name      : CountDownLatch.hpp
// brief     : 倒计时门闩同步类
// attention : None
// date      : 2023/10/14
//***********************************************
class CountDownLatch : noncopyable
{
 public:
	explicit CountDownLatch(int count) : m_count(count), m_cond(m_mutex)
	{
	}
	/// @brief 阻塞线程，等待所有条件满足
	void wait()
	{
		MutexLockGuard lock(m_mutex);
		while (m_count > 0)
		{
			m_cond.wait();
		}
	}
	/// @brief 递减条件计数器
	void coutDown()
	{
		MutexLockGuard lock(m_mutex);
		--m_count;
		if (0 == m_count)
		{
			m_cond.notifyAll();
		}
	}

	int getCount() noexcept
	{
		MutexLockGuard lock(m_mutex);
		return m_count;
	}

 private:
	/// @brief 计数器
	int m_count;

	MutexLock m_mutex;
	Condition m_cond;
};
}