#pragma once
#include <functional>
#include <chrono>
#include <memory>
#include <set>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <cassert>
#include "base/type.h"
#include "base/noncopyable.hpp"
#include "base/Macro.h"

namespace uf{

class TimerManager;
class Timer :public std::enable_shared_from_this<Timer>
{
friend class TimerManager;
public:
    using TimerCallback = std::function<void ()>;
    using ptr = std::shared_ptr<Timer>;

    void refresh() noexcept;

    void cancel() noexcept;

    /// @brief 定时器过期时间
    ATTR_PURE_INLINE
    std::chrono::nanoseconds Expiration() const noexcept
    {
        return m_expiration;
    }

    ATTR_PURE_INLINE
    bool isRepeat() const noexcept
    {
        return m_isRepeat;
    }

	ATTR_PURE_INLINE
	bool isCanceled() const noexcept
	{
		return m_isCanceled.load();
	}

    ATTR_PURE_INLINE
    std::chrono::milliseconds getRepeatTime() const
    {
        return m_repeatTime;
    }


private:
	Timer(std::reference_wrapper<TimerManager> manager ,TimerCallback cb,std::chrono::nanoseconds when,std::chrono::milliseconds repeat);

    void run() const
    {
        std::invoke(m_cb);
    }

    void setExpirationTime(std::chrono::nanoseconds now) noexcept
    {
        m_expiration = now;
    }

    struct Comparator
    {
        bool operator()(const Timer::ptr& lhs,const Timer::ptr& rhs) const ;
    };

    std::reference_wrapper<TimerManager> m_manager;
    TimerCallback m_cb;
	/// @brief 到期时间[ns]
	std::chrono::nanoseconds m_expiration;
    /// @brief 循环周期[ms]
	std::chrono::milliseconds m_repeatTime;
	bool m_isRepeat;
	std::atomic<bool> m_isCanceled;


};


class EventLoop;
class Channel;
//***********************************************
// name      : TimerManager
// brief     : 定时器管理类
// attention : None
//***********************************************

class TimerManager: noncopyable
{
friend class Timer;
public:
    using TimerCallback = std::function<void ()>;
	using ClockType = std::chrono::steady_clock;
	using TimePoint = std::chrono::time_point<ClockType,std::chrono::nanoseconds>;

    explicit TimerManager(std::reference_wrapper<EventLoop> loop);

    ~TimerManager();

    /// @brief 添加定时器
    /// @param cb 定时器回调函数
    /// @param when 定时器过期时间[ns]
    /// @param interval 定时器间隔时间[ms]，为0则定时器单执行一次
    /// @return 定时器对象
    Timer::ptr addTimer(const TimerCallback& cb,const TimePoint& when, const std::chrono::milliseconds& interval);

private:
	void insert(const Timer::ptr& timer) noexcept;
    void ResetTimer(std::chrono::nanoseconds now) const noexcept;
    void cancel(const Timer::ptr& timer) noexcept;
    void HandleRead() noexcept;
    std::vector<Timer::ptr> getExpiredTimer(std::chrono::nanoseconds now);

    std::reference_wrapper<EventLoop> m_loop;
    socket_t m_fd;
    std::unique_ptr<Channel> m_channel;
    std::set<Timer::ptr,Timer::Comparator> m_timers;
    std::atomic<bool> m_HandlingExpiredTimer;
    Timer::ptr m_sentry;//哨兵

};




}