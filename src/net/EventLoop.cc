#include "EventLoop.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <cassert>
#include "EventDispatcher.h"
#include "Channel.h"
#include "log/Logger.h"
// TODO: Update to use new config API
// #include "config/config.h"
#include "Util.h"
#include "Socket.h"

namespace bus
{

static Logger& g_logger = GET_LOGGER("sys");
//  [net.EventLoop]
//  timeoutMs = 60000
//static auto g_config = GET_ROOT_CONFIG("net.EventLoop").asTable();
// TODO: Update to use new config API
// static auto g_config = GET_CONFIG("root");

/// @brief 保证one loop per thread
thread_local EventLoop* t_ThisThreadLoop = nullptr;

namespace detail{

static socket_t CreateWakeupFd()
{
    socket_t fd = ::eventfd(0,EFD_CLOEXEC | EFD_NONBLOCK);
    if(fd < 0)
    {
        LOG_FATAL(g_logger) << "CreateWakeupFd error";
        SystemUtil::Abort();
    }
    return fd;
}

}

EventLoop::EventLoop()
        : m_dispatcher(new EpollEventDispatcher(this)),
          m_ThreadId(std::this_thread::get_id()),
          m_WakeupChannel(std::make_unique<Channel>(std::ref(*this),detail::CreateWakeupFd())),
          m_looping(false)
{
    assert(t_ThisThreadLoop == nullptr);
    t_ThisThreadLoop = this;
    //setStatus(Status::Initializing);

    m_WakeupChannel->setReadCallback([this](){
        WakeupRead();
    });
    m_WakeupChannel->EnableReadEvent();
    //setStatus(Status::Initialized);
}

EventLoop::~EventLoop()
{
    //setStatus(Status::Stopping);
    LOG_INFO(g_logger)<<"EventLoop "<<this<<" of thread" << m_ThreadId<<" destruct in thread"
                        << std::this_thread::get_id();
    m_WakeupChannel->DisableAllEvent();
    //RemoveChannel(m_WakeupChannel.get());
    ::close(m_WakeupChannel->fd());
    t_ThisThreadLoop = nullptr;
    //setStatus(Status::Stopped);
}

void EventLoop::Loop()
{
    assert(isInLoopThread());
	//assert(m_status == Status::Initialized || m_status == Status::Stopped);
    assert(!m_looping);
    // if(getStatus() == Status::Running){
    //     LOG_WARN(g_logger)<<"EventLoop :"<< this <<", already running.";
    //     return;
    // }
    
    m_looping = true;
    EventDispatcher::ChannelList activeChannel;
    //auto timeoutMs= g_config->getConfigValue("net.EventLoop").asTable()->Lookup("timeoutMs").asInteger().value_or(60*1000);
    // TODO: Update to use new config API
    auto timeoutMs = 60*1000;  // Default value
    LOG_TRACE(g_logger)<<"EventLoop :"<<this <<" looping , timeout = " << timeoutMs;
    //setStatus(Status::Running);
    while(m_looping)
    {
        activeChannel.clear();
        m_dispatcher->Dispatcher(static_cast<int>(timeoutMs),activeChannel);
        for(const auto& iter : activeChannel)
        {
            //处理事件回调
            iter->HandleEvent();
        }
        //执行PendingFunctor
        doPendingFunctor();
    }
    LOG_TRACE(g_logger)<<"EventLoop :"<<this <<" is stopped";
    //setStatus(Status::Stopped);
}
void EventLoop::UpdateChannel(Channel* channel) noexcept
{
	assert(isInLoopThread());
	assert(channel->OwnerLoop() == this);
	m_dispatcher->UpdateChannel(channel);
}
void EventLoop::RemoveChannel(Channel* channel) noexcept
{
	assert(isInLoopThread());
	assert(channel->OwnerLoop() == this);
	m_dispatcher->RemoveChannel(channel);
}
void EventLoop::Stop()
{
    //setStatus(Status::Stopping);
    RunInLoop([this](){
        m_looping = false;
    });
}

void EventLoop::RunInLoop(const Functor& cb)
{
    if(isInLoopThread())
    {
        std::invoke(cb);
    }
    else
    {
        QueueInLoop(cb);
    }
}

void EventLoop::QueueInLoop(const EventLoop::Functor &cb)
{
    {
        std::lock_guard lock(m_mutex);
        m_PendingFunctor.push_back(cb);
    }
    Wakeup();
}

void EventLoop::doPendingFunctor()
{
    FunctorList pendingFunctor;
    {
        std::lock_guard lock(m_mutex);
        std::swap(pendingFunctor,m_PendingFunctor);
    }
    for(const auto& cb:pendingFunctor)
    {
        std::invoke(cb);
    }
}

void EventLoop::WakeupRead() noexcept
{
    uint64_t one{1};
    ssize_t n;
    do
    {
        n = ::read(m_WakeupChannel->fd(),&one,sizeof one);
    }while(n < 0 && errno == EINTR);   // M5：仅失败时检查 errno——成功后残留
                                       // EINTR 会二次 read 得 -1 并误报 ERROR

    if(n != sizeof one)
    {
        LOG_ERROR(g_logger)<<"WakeupRead() , read " << n <<"bytes instead of 8";
    }
}

void EventLoop::Wakeup() const noexcept
{
    uint64_t one{1};
    ssize_t n = ::write(m_WakeupChannel->fd(),&one,sizeof one);
    if(n != sizeof one)
    {
        LOG_ERROR(g_logger)<<"Wakeup() , write " << n <<"bytes instead of 8";
    }
}


}