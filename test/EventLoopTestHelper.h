//***********************************************
// name      : EventLoopTestHelper.h
// brief     : 事件循环测试辅助设施（Loop 线程、同步投递）
// attention : EventLoop 的创建、Loop、销毁均须在 loop 线程完成；
//             LoopThread 析构会请求 Stop 并 join，Stop 前须保证
//             loop 线程内不再有依赖栈上对象的待执行任务
//***********************************************
#pragma once
#include "net/EventLoop.h"
#include <atomic>
#include <future>
#include <thread>
#include <type_traits>
#include <utility>

namespace bus {
namespace test {

/// @brief 在独立线程中创建并运行 EventLoop
/// @attention EventLoop 的创建、Loop、销毁均在该线程完成；析构与用户自行
///             Stop() 并发时，线程会在 Loop() 返回后阻塞等待析构方的放行
///             信号，确保 Stop() 不解引用已销毁的 EventLoop
class LoopThread {
 public:
    LoopThread()
        : m_thread([this]() {
              EventLoop loop;
              m_loop.store(&loop);
              m_ready.set_value(&loop);
              loop.Loop();
              // Loop() 已返回：通知析构方，并在放行前保持 loop 存活，
              // 消除"析构方 Stop() 读到非空指针但对象正析构"的窗口
              m_stopped.set_value();
              m_may_finish.get_future().wait();
              m_loop.store(nullptr);
          }) {
        m_ready.get_future().wait();
    }

    ~LoopThread() {
        Stop();
        if (m_stopped_future.valid()) {
            m_stopped_future.wait();
        }
        m_may_finish.set_value();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;

    /// @brief 获取 loop 指针（在 Stop 前有效）
    ATTR_PURE_INLINE
    EventLoop* loop() const noexcept { return m_loop.load(); }

    /// @brief 请求停止事件循环（幂等；Loop 已退出时为安全空操作）
    void Stop() noexcept {
        EventLoop* loop = m_loop.load();
        if (loop != nullptr) {
            loop->Stop();  // 线程在 Loop 返回后会阻塞等待 m_may_finish，对象仍存活
        }
    }

 private:
    /// @brief loop 就绪通知
    std::promise<EventLoop*> m_ready;
    /// @brief Loop() 返回通知（析构方等待后放行）
    std::promise<void> m_stopped;
    std::future<void> m_stopped_future = m_stopped.get_future();
    /// @brief 析构方放行信号（此后线程方可置空指针并析构 EventLoop）
    std::promise<void> m_may_finish;
    /// @brief loop 指针
    std::atomic<EventLoop*> m_loop{nullptr};
    /// @brief 运行 loop 的线程
    std::thread m_thread;
};

/// @brief 在 loop 线程同步执行函数并等待结果
/// @param loop 目标事件循环
/// @param fn 待执行函数
/// @return fn 的返回值
/// @attention 仅可在 loop 正在运行时调用，否则会阻塞
template <typename Fn>
auto RunInLoopSync(EventLoop* loop, Fn&& fn) -> decltype(fn()) {
    using Result = decltype(fn());
    std::promise<Result> promise;
    auto future = promise.get_future();
    loop->RunInLoop([&promise, fn = std::forward<Fn>(fn)]() mutable {
        try {
            if constexpr (std::is_void_v<Result>) {
                fn();
                promise.set_value();
            } else {
                promise.set_value(fn());
            }
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    return future.get();
}

}  // namespace test
}  // namespace bus
