#include "Channel.h"
#include "log/Logger.h"
#include "EventLoop.h"
#include "Socket.h"
#include <cassert>

namespace bus{

static Logger& g_logger = GET_LOGGER("system");

namespace detail{

static std::string EventToString(event_t event)
{
    using namespace EventType;
    std::string str;
    if(event & kReadable)
    {
        str.assign("kReadable");
    }
    if(event & kWritable)
    {
        if(!str.empty())
        {
            str.push_back('|');
        }
        str.append("kWritable");
    }
    if(event & kError)
    {
        if(!str.empty())
        {
            str.push_back('|');
        }
        str.append("kError");
    }
    return str;
}

}

Channel::Channel(View<EventLoop> loop,socket_t fd)
: m_loop(std::move(loop)),
  m_fd(fd),
  m_ListeningEvents(EventType::kNone),
  m_OccurEvents(EventType::kNone)
{

}
void Channel::HandleEvent() const
{
    // D27 存活守卫：宿主已亡则事件作废；存活则分派期间宿主（连带本
    // Channel）不可析构，分派循环的裸指针访问安全
    std::shared_ptr<const void> guard;
    if (m_tied)
    {
        guard = m_tie.lock();
        if (!guard)
        {
            return;
        }
    }

    LOG_TRACE(g_logger)<< "fd=" << m_fd <<",event="<<detail::EventToString(m_OccurEvents);

    using namespace EventType;
    if((m_OccurEvents & kReadable) && m_ReadCallback)
    {
        std::invoke(m_ReadCallback);
    }
    if((m_OccurEvents & kWritable) && m_WriteCallback)
    {
        std::invoke(m_WriteCallback);
    }
    if((m_OccurEvents & kError) && m_ErrorCallback)
    {
        std::invoke(m_ErrorCallback);
    }
}

void Channel::update() noexcept
{
	m_loop.Peer().UpdateChannel(this);
}
void Channel::EnableWriteEvent() noexcept
{
	m_ListeningEvents |= EventType::kWritable;
	update();
}
void Channel::EnableReadEvent() noexcept
{
	m_ListeningEvents |= EventType::kReadable;
	update();
}
void Channel::DisableReadEvent() noexcept
{
	m_ListeningEvents &= ~EventType::kReadable;
	update();
}
void Channel::DisableWriteEvent() noexcept
{
	m_ListeningEvents &= ~EventType::kWritable;
	update();
}
void Channel::DisableAllEvent() noexcept
{
	m_ListeningEvents = EventType::kNone;
	update();
}

}