#include "Timer.h"
#include <unistd.h>
#include <sys/timerfd.h>
#include <utility>
#include <cassert>
#include <iomanip>
#include <iostream>
#include "log/Logger.h"
#include "exception//Exception.hpp"
#include "net/Channel.h"
#include "net/EventLoop.h"

namespace bus{

using namespace std::literals;

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

void setTimer(socket_t fd,std::chrono::nanoseconds expiration,std::chrono::nanoseconds now)
{
    itimerspec newValue{};
    //itimerspec oldValue{};

    auto waitTime = expiration - now;
    using namespace chrono_literals
    ;
    if(waitTime < 100ms)
    {
        waitTime = 100ms;
    }
    assert(waitTime.count()>0);
    newValue.it_value.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(waitTime).count();
    newValue.it_value.tv_nsec = (waitTime % 1s).count();

	LOG_INFO(g_logger)<<"set timer,sec="<<newValue.it_value.tv_sec<<" nsec="<<newValue.it_value.tv_nsec;
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

void TimerManager::insert(const Timer::ptr& timer) noexcept
{
    assert(m_loop.get().isInLoopThread());
    if(m_timers.empty() || timer->Expiration() < (*m_timers.begin())->Expiration())
    {
        detail::setTimer(m_fd,timer->Expiration(),std::chrono::steady_clock::now().time_since_epoch());
    }
    auto [iter,OK] = m_timers.insert(timer);
    assert(OK);
	LOG_TRACE(g_logger)<<"insert timer,expiration="<<(*iter)->Expiration().count()<<"ns";
}

TimerManager::TimerManager(std::reference_wrapper<EventLoop> loop)
:m_loop(loop),
 m_fd(detail::CreateTimerFd()),
 m_channel(std::make_unique<Channel>(m_loop,m_fd)),
 m_timers(),
 m_HandlingExpiredTimer(false),
 m_sentry(new Timer(std::ref(*this), nullptr,0ns,0ms))
{
    m_channel->setReadCallback([this](){
        m_HandlingExpiredTimer.store(true);
        HandleRead();
        m_HandlingExpiredTimer.store(false);
    });
    m_channel->EnableReadEvent();
}

void TimerManager::HandleRead() noexcept
{
    assert(m_loop.get().isInLoopThread());
    std::chrono::nanoseconds now = ClockType::now().time_since_epoch();
    detail::readTimerFd(m_fd,std::chrono::duration_cast<std::chrono::seconds>(now).count());

    auto expiredTimer = getExpiredTimer(now);
    for (auto&& timer: expiredTimer)
    {
		if(!timer->isCanceled())
        {
			LOG_TRACE(g_logger)<<"handle expired timer,expired="<<timer->Expiration().count()<<"ns";
            timer->run();
            if(timer->isRepeat())
            {
                timer->setExpirationTime(timer->Expiration() + timer->getRepeatTime());
                auto [iter,OK] = m_timers.insert(std::move(timer));
                assert(OK);
                LOG_TRACE(g_logger)<<"insert repeat timer,expiration="<<(*iter)->Expiration().count()<<"ns";
            }
        }
    }
    //TODO:定时间隔是否去除调用时间？
    ResetTimer(now);
}

std::vector<Timer::ptr> TimerManager::getExpiredTimer(std::chrono::nanoseconds now)
{
    assert(m_loop.get().isInLoopThread());
    m_sentry->setExpirationTime(now);
    auto lowerSentry = m_timers.lower_bound(m_sentry);
    assert(lowerSentry == m_timers.cend() || now < (*lowerSentry)->Expiration());

    std::vector<Timer::ptr> expiredTimer;
    std::move(m_timers.begin(),lowerSentry,std::back_inserter(expiredTimer));
    assert(expiredTimer.size() == std::distance(m_timers.begin(),lowerSentry));
    auto eraseIter = m_timers.erase(m_timers.begin(),lowerSentry);
    assert(eraseIter == lowerSentry);

    return std::move(expiredTimer);
}
void TimerManager::cancel(const Timer::ptr& timer) noexcept
{

	LOG_DEBUG(g_logger)<<"cancel timer,expiration="<<timer->Expiration().count()<<"ns";

    m_loop.get().RunInLoop([this,&loop=m_loop.get(),timer](){
        assert(loop.isInLoopThread());
		if(auto iter = m_timers.find(timer);iter != m_timers.cend())
		{
			m_timers.erase(iter);
		}
    });

}

Timer::ptr TimerManager::addTimer(const TimerManager::TimerCallback& cb,
                                  const TimePoint &when,
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

void TimerManager::ResetTimer(std::chrono::nanoseconds now) const noexcept
{
    assert(m_loop.get().isInLoopThread());
    if(!m_timers.empty())
    {
        assert(std::all_of(m_timers.begin(),m_timers.end(),[this](auto& i){
            return (*m_timers.begin())->Expiration() <= i->Expiration();
        }));
        detail::setTimer(m_fd,(*m_timers.begin())->Expiration(),now);
    }

}

//对于不完整类型使用uniq_ptr，所属class须声明析构，但实现在其他位置
TimerManager::~TimerManager()=default;

Timer::Timer(std::reference_wrapper<TimerManager> manager, Timer::TimerCallback cb, std::chrono::nanoseconds when,
             std::chrono::milliseconds repeat)
: m_manager(manager),
  m_cb(std::move(cb)),
  m_expiration(when),
  m_repeatTime(repeat),
  m_isRepeat(m_repeatTime.count() >0),
  m_isCanceled(false)
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
	if(!m_isCanceled.exchange(true))
	{
		m_manager.get().cancel(shared_from_this());
	}
}

bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const
{
    return lhs->m_expiration < rhs->m_expiration;
}

}