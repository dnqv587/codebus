#pragma once
#include <functional>
#include "base/type.h"
#include "base/view.h"
#include "time/Timestamp.h"

namespace uf{

//事件类型
namespace EventType{
extern const event_t kNone;
extern const event_t kReadable;
extern const event_t kWritable;
extern const event_t kError;
}

class EventLoop;
//***********************************************
// name      : Channel
// brief     : 负责维护fd的注册事件与响应事件，并进行事件分派
// attention : 不拥有fd的生命周期；每个Channel只能属于一个loop（IO线程）
//***********************************************
class Channel
{
public:
	using WritrEventCallback = std::function<void ()>;
	using ReadEventCallback =  std::function<void ()>;
    using ErrorEventCallback =  std::function<void ()>;

    Channel(std::reference_wrapper<EventLoop> loop,socket_t fd);
	~Channel()=default;
    /// @brief 将发生事件进行分派
    /// @param fd 产生事件的fd
    /// @param event  发生的事件集合
	void HandleEvent() const;

    void setReadCallback(const ReadEventCallback& cb)
    {m_ReadCallback=cb;}
    void setWriteCallback(const WritrEventCallback& cb)
    {m_WriteCallback=cb;}
    void setErrorCallback(const ErrorEventCallback& cb)
    {m_ErrorCallback=cb;}

	/// @brief 监听可读
    void EnableReadEvent() noexcept;
	/// @brief 监听可写
    void EnableWriteEvent() noexcept;

	void DisableReadEvent() noexcept;
	void DisableWriteEvent() noexcept;
	void DisableAllEvent() noexcept;


	/// @brief 获取监听事件
	/// @return event
	ATTR_PURE_INLINE
	event_t ListeningEvent() const noexcept
	{return m_ListeningEvents;}
	/// @brief 获取发生事件
	/// @return event
	ATTR_PURE_INLINE
	event_t OccurEvent() const noexcept
	{return m_OccurEvents;}
	/// @brief 设置发生事件
	/// @param event
	ATTR_INLINE
	void setOccurEvent(event_t event)
	{m_OccurEvents = event;}
    ATTR_PURE_INLINE
    socket_t fd() const noexcept
    {return m_fd;}
	/// @brief 所属的Loop，每个Channel只属于一个Loop
	/// @return Loop
	ATTR_PURE_INLINE
	EventLoop* OwnerLoop() const noexcept
	{return m_loop.NativeHandle();}

private:
	void update() noexcept;

    View<EventLoop> m_loop;
	//文件描述符，可能是socket、eventfd或timerfd
    socket_t m_fd;

	event_t m_ListeningEvents;//监听事件集合
	event_t m_OccurEvents;//发生事件集合

	//回调
	ReadEventCallback m_ReadCallback;
    WritrEventCallback m_WriteCallback;
    ErrorEventCallback m_ErrorCallback;
};
}