#pragma once
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include "base/noncopyable.hpp"
#include "base/Macro.h"
#include "ServerStatus.h"

namespace bus{

class Channel;
class EventDispatcher;
class EventLoop:ServerStatus<true> , noncopyable
{
public:
    using Functor=std::function<void ()>;

    EventLoop();
    ~EventLoop();

    void Loop();

    void Stop();

	void UpdateChannel(Channel* channel) noexcept;

	void RemoveChannel(Channel* channel) noexcept;

	/// @brief 当前是否为IO线程，提高线程安全性
	/// @return true/false
    ATTR_PURE_INLINE
    bool isInLoopThread() const noexcept
    {return m_ThreadId == std::this_thread::get_id();}

    /// @brief 在IO线程中执行函数，避免资源竞争
    /// @param cb 待执行函数
	void RunInLoop(const Functor& cb);

    /// @brief 唤醒被阻塞的IO线程
    void Wakeup() const noexcept;

private:
    void QueueInLoop(const Functor& cb);
    void doPendingFunctor();
    void WakeupRead() noexcept;

    using FunctorList=std::vector<Functor>;

    std::unique_ptr<EventDispatcher> m_dispatcher;
    std::thread::id m_ThreadId;
    std::unique_ptr<Channel> m_WakeupChannel;
    bool m_looping;
    std::mutex m_mutex;
    FunctorList m_PendingFunctor;

};
}