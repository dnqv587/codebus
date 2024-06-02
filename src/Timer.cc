#include "Timer.h"
#include <unistd.h>
#include <sys/timerfd.h>
#include <utility>
#include <cassert>
#include <iomanip>
#include "log/Logger.h"
#include "exception//Exception.hpp"
#include "net/Channel.h"
#include "net/EventLoop.h"

namespace uf{

Logger& g_logger = GET_LOGGER("system");

namespace detail{

socket_t CreateTimerFd()
{
	int fd = ::timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK | TFD_CLOEXEC);
	if(fd < 0)
	{
		LOG_ERROR(g_logger)<<"CreateTimerFd error,err="<<strerror(errno);
		throw Exception::SocketError("CreateTimerFd error");
	}
	return fd;
}

void setTimer(socket_t fd,std::chrono::nanoseconds expiration)
{
    itimerspec newValue{};
    //itimerspec oldValue{};

    auto waitTime = expiration - std::chrono::system_clock::now().time_since_epoch();
    newValue.it_value.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(waitTime).count();
    newValue.it_value.tv_nsec = (waitTime % std::chrono::seconds(1)).count();

    int ret = ::timerfd_settime(fd ,0 ,&newValue, nullptr);
    if(ret < 0)
    {
        LOG_ERROR(g_logger)<<"setTimer error,err="<<strerror(errno);
    }
}

void readTimerFd(socket_t fd,std::time_t now)
{
    uint64_t eight{};
    auto n = ::read(fd,&eight,sizeof(eight));
    LOG_TRACE(g_logger)<<"Timer read "<<eight << ",fd="<<fd<<",at ="
                    <<std::put_time(std::localtime(&now),"%F %T");
    if(n != sizeof(eight))
    {
        LOG_ERROR(g_logger)<<"Timer reads " << n << " bytes instead of 8";
    }
}


}

void TimerManager::insert(const Timer::ptr& timer)
{
    assert(m_loop.get().isInLoopThread());
    if(m_timers.empty() || timer->Expiration() < (*m_timers.begin())->Expiration())
    {
        detail::setTimer(m_fd,timer->Expiration());
    }
    auto [iter,OK] = m_timers.insert(timer);
    assert(OK);
}

TimerManager::TimerManager(std::reference_wrapper<EventLoop> loop)
:m_loop(loop),
 m_fd(detail::CreateTimerFd()),
 m_channel(std::make_unique<Channel>(m_loop,m_fd)),
 m_timers(),
 m_cancelTimers(),
 m_HandlingExpiredTimer(false),
 m_cancelMutex()
{
    m_channel->setReadCallback([this](){
        m_HandlingExpiredTimer.store(true);
        HandleRead();
        m_HandlingExpiredTimer.store(false);
    });
    m_channel->EnableReadEvent();
}

void TimerManager::HandleRead()
{
    assert(m_loop.get().isInLoopThread());
    std::chrono::nanoseconds now = std::chrono::system_clock::now().time_since_epoch();
    detail::readTimerFd(m_fd,std::chrono::duration_cast<std::chrono::seconds>(now).count());

    auto expiredTimer = getExpiredTimer(now);
    for (const auto& timer: expiredTimer)
    {
        std::unique_lock lock(m_cancelMutex,std::defer_lock);
        lock.lock();
        //timer是否被撤销
        if(m_cancelTimers.find(timer) == m_cancelTimers.cend())
        {
            lock.unlock();
            timer->run();
            if(timer->isRepeat())
            {
                timer->setExpirationTime(now + timer->getRepeatTime());
                insert(timer);
            }
        }
        else
        {
            lock.unlock();
        }
    }
}

std::vector<Timer::ptr> TimerManager::getExpiredTimer(std::chrono::nanoseconds now)
{
    assert(m_loop.get().isInLoopThread());
    Timer::ptr sentry (new Timer(std::ref(*this), nullptr,now,std::chrono::milliseconds(0)));
    auto lowerSentry = m_timers.lower_bound(sentry);
    assert(lowerSentry == m_timers.cend() || now < (*lowerSentry)->Expiration());

    std::vector<Timer::ptr> expiredTimer;
    std::move(m_timers.begin(),lowerSentry,std::back_inserter(expiredTimer));
    assert(expiredTimer.size() == std::distance(m_timers.begin(),lowerSentry));
    auto eraseIter = m_timers.erase(m_timers.begin(),lowerSentry);
    assert(eraseIter == lowerSentry);

    return expiredTimer;
}
void TimerManager::cancel(Timer::ptr timer)
{
    m_loop.get().RunInLoop([this,&loop=m_loop.get(),timer](){
        assert(loop.isInLoopThread());
        auto n = m_timers.erase(timer);
        assert(n == 1);
    });
    //正在处理逾期timer，将撤销的timer插入到cancelTimers
    if(m_HandlingExpiredTimer)
    {
        std::lock_guard lock(m_cancelMutex);
        m_cancelTimers.insert(std::move(timer));
    }
}

Timer::ptr TimerManager::addTimer(const TimerManager::TimerCallback& cb,
                                  const std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> &when,
                                  const std::chrono::milliseconds &interval)
{
    //Timer::ptr timer = std::make_shared<Timer>(std::ref(*this),cb,when.time_since_epoch(),interval);
    Timer::ptr timer(new Timer(std::ref(*this),cb,when.time_since_epoch(),interval));
    m_loop.get().RunInLoop([this, timer](){
                               assert(m_loop.get().isInLoopThread());
                               insert(timer);
                           });
    return timer;

}

//对于不完整类型使用uniq_ptr，所属class须声明析构，但实现在其他位置
TimerManager::~TimerManager()=default;

Timer::Timer(std::reference_wrapper<TimerManager> manager, Timer::TimerCallback cb, std::chrono::nanoseconds when,
             std::chrono::milliseconds repeat)
:m_manager(manager),
m_cb(std::move(cb)),
 m_expiration(when),
 m_repeatTime(repeat),
 m_isRepeat(m_repeatTime.count() >0)
{

}

void Timer::refresh() noexcept
{
    if(m_isRepeat)
    {
        m_manager.get().m_loop.get().RunInLoop([&loop = m_manager.get().m_loop.get(),
												&manager=m_manager.get(),
												timer = shared_from_this()]()->auto{
			assert(loop.isInLoopThread());
		  	timer->m_expiration = std::chrono::system_clock::now().time_since_epoch() + timer->m_repeatTime;
			manager.cancel(timer);
            manager.insert(timer);
        });
    }
}

void Timer::cancel() noexcept
{
    m_manager.get().m_loop.get().RunInLoop([&loop = m_manager.get().m_loop.get(),
                                                   &manager=m_manager.get(),
                                                   timer = shared_from_this()](){
        assert(loop.isInLoopThread());
        manager.cancel(timer);
    });
}

bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const
{
    return lhs->m_expiration < rhs->m_expiration;
}

}