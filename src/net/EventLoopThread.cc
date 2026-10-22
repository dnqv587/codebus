//***********************************************
// name      : EventLoopThread.cc
// brief     : IO 线程与 IO 线程池实现（线程协议移植自测试侧已验证的 LoopThread）
// attention : None
//***********************************************
#include "net/EventLoopThread.h"
#include "net/EventLoop.h"

#include <cassert>
#include <utility>

namespace bus
{

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb, std::string name)
    : m_init_cb(cb),
      m_thread([this]() { threadFunc(); }, std::move(name))
{
}

EventLoopThread::~EventLoopThread()
{
    if (!m_started)
    {
        return;   // 从未启动：线程未 run，无须等待
    }
    EventLoop* loop = m_loop.load();
    if (loop != nullptr)
    {
        // 线程在 Loop() 返回后阻塞等待 m_may_finish，EventLoop 仍存活
        loop->Stop();
    }
    m_stopped_future.wait();
    m_may_finish.set_value();
    m_thread.join();
}

EventLoop* EventLoopThread::StartLoop()
{
    assert(!m_started);
    m_started = true;
    m_thread.run();
    return m_ready.get_future().get();
}

void EventLoopThread::threadFunc()
{
    EventLoop loop;
    m_loop.store(&loop);
    m_ready.set_value(&loop);
    if (m_init_cb)
    {
        m_init_cb(&loop);
    }
    loop.Loop();
    // Loop() 已返回：通知宿主，并在放行前保持 loop 存活，消除
    // “宿主 Stop() 读到非空指针但对象正析构”的窗口
    m_stopped.set_value();
    m_may_finish.get_future().wait();
    m_loop.store(nullptr);
}

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : m_base_loop(baseLoop),
      m_name(std::move(name))
{
    assert(m_base_loop != nullptr);
}

void EventLoopThreadPool::Start(const ThreadInitCallback& cb)
{
    if (m_started.exchange(true))
    {
        return;   // 幂等：重复 Start 无效果
    }
    for (uint32_t i = 0; i < m_thread_num; ++i)
    {
        auto thread = std::make_unique<EventLoopThread>(cb, m_name + "-" + std::to_string(i));
        m_loops.push_back(thread->StartLoop());
        m_threads.push_back(std::move(thread));
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() noexcept
{
    EventLoop* loop = m_base_loop;
    if (!m_loops.empty())
    {
        loop = m_loops[m_next.fetch_add(1, std::memory_order_relaxed) % m_loops.size()];
    }
    return loop;
}

} // namespace bus
