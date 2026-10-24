#pragma once
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include "noncopyable.hpp"
#include "Macro.h"

namespace bus{

class Channel;
class EventDispatcher;
class EventLoop: noncopyable
{
public:
    using Functor=std::function<void ()>;
    using ref = std::reference_wrapper<EventLoop>;

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

    /// @brief 无条件排队到 loop（在 loop 线程也只排队、不立即执行，
    ///        于下一轮 doPendingFunctor 即事件分派循环之外运行）
    /// @param cb 待执行函数
    /// @attention 供「执行体须避开当前分派轮」的场景使用（如 TcpServer::Stop
    ///            的关闭链——同步销毁 pending 中的 Channel 会造成同轮悬垂）
	void QueueInLoop(const Functor& cb);

    /// @brief 唤醒被阻塞的IO线程
    void Wakeup() const noexcept;

private:
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