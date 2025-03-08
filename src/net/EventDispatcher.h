#pragma once
#include <vector>
#include <unordered_map>
#include "base/view.h"
#include "base/type.h"


struct epoll_event;
namespace bus{

class EventLoop;
class Channel;
//***********************************************
// name      : EventDispatcher
// brief     : IO Multiplexing封装类
// attention : None
//***********************************************
class EventDispatcher
{
public:
    using ChannelList=std::vector<Channel*>;

    explicit EventDispatcher(EventLoop* loop);
    virtual ~EventDispatcher()=default;

    /// @brief
    /// @param timeoutMs 超时时间[ms]
    ///        -1：永久等待事件
    ///         0：立即返回
    ///        >0：定时等待事件
    ///	@param active_channel [out] 有事件发生的channel
    virtual void Dispatcher(int timeoutMs,ChannelList& active_channel) = 0;

    /// @brief 增加监听Channel
    virtual void UpdateChannel(Channel* channel) = 0;
    /// @brief 取消监听Channel
    virtual void RemoveChannel(Channel* channel) = 0;
protected:
    using ChannleMap = std::unordered_map<socket_t ,Channel*>;
	/// @brief 判断channel是否正在被监听
	/// @param channel
	/// @return true/false
	bool isListeningChannel(Channel* channel) const;
	ATTR_PURE_INLINE
	EventLoop* getOwnerLoop() const noexcept
	{return m_OwnerLoop.NativeHandle();}

    //正在监听的Channel集合
    ChannleMap m_ListeningChannelMap;
private:
    View<EventLoop> m_OwnerLoop;//所属的Loop
};


class EpollEventDispatcher:public EventDispatcher
{
 public:
	explicit EpollEventDispatcher(EventLoop* loop);
	~EpollEventDispatcher() override =default;

	void Dispatcher(int timeoutMs,ChannelList& active_channel) override;
	void UpdateChannel(Channel* channel) override;
	void RemoveChannel(Channel* channel) override;

 private:
	void Update(int operation ,Channel* channel) const ;

	using EventList = std::vector<struct epoll_event>;
	socket_t m_EpollFd;
	EventList m_OccurEvent;//发生事件列表
};

}