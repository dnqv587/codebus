#include "EventDispatcher.h"
#include <sys/epoll.h>
#include <cassert>
#include "log/Logger.h"
#include "Channel.h"
#include "EventLoop.h"

namespace uf
{
static Logger &g_logger = GET_LOGGER("system");
constexpr static int kInitEventListSize = 16;

const event_t EventType::kNone 		= NULL;
const event_t EventType::kReadable 	= EPOLLIN | EPOLLPRI; //| EPOLLRDHUP;
const event_t EventType::kWritable	= EPOLLOUT;
const event_t EventType::kError 	= EPOLLERR;

namespace detail{
static std::string EpollOperationString(int operation)
{
    switch(operation)
    {
        case EPOLL_CTL_ADD:
            return "EPOLL_CTL_ADD";
        case EPOLL_CTL_MOD:
            return "EPOLL_CTL_MOD";
        case EPOLL_CTL_DEL:
            return "EPOLL_CTL_DEL";
	default:
		return "";
    }
}
}

EventDispatcher::EventDispatcher(EventLoop* loop)
        : m_ListeningChannelMap{},
		  m_OwnerLoop{std::ref(*loop)}
{
}
bool EventDispatcher::isListeningChannel(Channel* channel) const
{
	return m_ListeningChannelMap.find(channel->fd()) != m_ListeningChannelMap.cend();
}

EpollEventDispatcher::EpollEventDispatcher(EventLoop* loop)
        : EventDispatcher{loop},
          m_EpollFd(::epoll_create1(EPOLL_CLOEXEC)),
          m_OccurEvent(kInitEventListSize)
{
    if (m_EpollFd < 0)
    {
        LOG_ERROR(g_logger) << "epoll_create error,errno=" << ::strerror(errno);
    }
}

void EpollEventDispatcher::Dispatcher(int timeoutMs, EventDispatcher::ChannelList& active_channel)
{
	struct epoll_event list[64]{0};

    int num_event = ::epoll_wait(m_EpollFd, m_OccurEvent.data(), static_cast<int>(m_OccurEvent.size()), timeoutMs);

	error_t err = errno;
    if (num_event > 0)
    {
        LOG_TRACE(g_logger) << num_event << " events happened";

        //处理发生事件Channel
		for(int i=0;i<num_event;++i)
		{
			auto* channel = static_cast<Channel*>(m_OccurEvent[i].data.ptr);
			assert(m_ListeningChannelMap[channel->fd()] == channel);
			channel->setOccurEvent(m_OccurEvent[i].events);
			active_channel.push_back(channel);
		}

        //扩容
        if (num_event >= m_OccurEvent.size())
        {
            m_OccurEvent.resize(m_OccurEvent.size() * 2);
        }
    } else if (num_event == 0)
    {
        LOG_TRACE(g_logger) << "epoll_wait nothing happened";
    } else
    {
        LOG_ERROR(g_logger) << "epoll_wait error,errno=" << ::strerror(err);
    }
}

void EpollEventDispatcher::UpdateChannel(Channel* channel)
{
	assert(getOwnerLoop()->isInLoopThread());
	if(isListeningChannel(channel))
	{
		//channel无监听事件，则去除
		if(channel->ListeningEvent() == EventType::kNone)
		{
			RemoveChannel(channel);
		}
		else	//已有channel，则更新
		{
			assert(m_ListeningChannelMap[channel->fd()] == channel);
			Update(EPOLL_CTL_MOD,channel);
		}
	}
	else
	{
		m_ListeningChannelMap.insert(std::make_pair(channel->fd(),channel));
		assert(m_ListeningChannelMap[channel->fd()] == channel);
		Update(EPOLL_CTL_ADD,channel);
	}
}

void EpollEventDispatcher::Update(int operation, Channel* channel) const
{
    epoll_event event{};
    event.events = channel->ListeningEvent();
    event.data.ptr = channel;

    LOG_TRACE(g_logger)<< "epoll_ctl Update ,operation="<< detail::EpollOperationString(operation)<<",fd="<<channel->fd();

    if(::epoll_ctl(m_EpollFd,operation,channel->fd(),&event)<0)
    {
        LOG_ERROR(g_logger)<< "epoll_ctl error,operation="<< detail::EpollOperationString(operation)<<",fd="<<channel->fd();
    }
}
void EpollEventDispatcher::RemoveChannel(Channel* channel)
{
	assert(getOwnerLoop()->isInLoopThread());
	assert(isListeningChannel(channel));
	assert(m_ListeningChannelMap[channel->fd()] == channel);
	assert(channel->ListeningEvent() == EventType::kNone);

	if(m_ListeningChannelMap.erase(channel->fd()) > 0)
	{
		Update(EPOLL_CTL_DEL,channel);
	}
	else
	{
		LOG_ERROR(g_logger)<<"RemoveChannel error,not found channel, fd="<<channel->fd();
	}
}

}
