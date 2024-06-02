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
class Timer :std::enable_shared_from_this<Timer>
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
	/// @brief 到期时间戳[ns]
	std::chrono::nanoseconds m_expiration;
    /// @brief 循环周期[ms]
	std::chrono::milliseconds m_repeatTime;
	bool m_isRepeat;


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

    explicit TimerManager(std::reference_wrapper<EventLoop> loop);

    ~TimerManager();

    /// @brief 添加定时器
    /// @param cb 定时器毁掉函数
    /// @param when 定时器过期时间
    /// @param interval 定时器间隔时间，为0则定时器单执行一次
    /// @return 定时器对象
    Timer::ptr addTimer(const TimerCallback& cb,const std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds>& when
                        , const std::chrono::milliseconds& interval = std::chrono::milliseconds(0));

private:
	void insert(const Timer::ptr& timer);
    void cancel(Timer::ptr timer);
    void HandleRead();
    std::vector<Timer::ptr> getExpiredTimer(std::chrono::nanoseconds now);

    std::reference_wrapper<EventLoop> m_loop;
    socket_t m_fd;
    std::unique_ptr<Channel> m_channel;
    std::set<Timer::ptr,Timer::Comparator> m_timers;
    std::unordered_set<Timer::ptr> m_cancelTimers;
    std::atomic<bool> m_HandlingExpiredTimer;
    std::mutex m_cancelMutex;

};




}