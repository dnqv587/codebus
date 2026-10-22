//***********************************************
// name      : EventLoopThread.h
// brief     : IO 线程：在独立线程中创建并运行一个 EventLoop（One Loop Per
//             Thread）；EventLoopThreadPool 管理 N 个 IO 线程并 round-robin 分发
// attention : EventLoopThread 的 StartLoop 仅可调用一次，且须与析构在同一
//             线程；析构会请求停止 loop 并 join，线程函数在放行前保持
//             EventLoop 存活（消除 Stop 与析构的竞态窗口）
//***********************************************
#pragma once
#include "base/noncopyable.hpp"
#include "base/Macro.h"
#include "thread/Thread.h"

#include <atomic>
#include <cassert>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace bus
{

class EventLoop;

class EventLoopThread : noncopyable
{
 public:
    /// @brief 线程初始化回调（loop 线程内、Loop() 之前调用一次）
    using ThreadInitCallback = std::function<void (EventLoop*)>;

    /// @brief 构造 IO 线程对象（不启动线程）
    /// @param cb loop 线程初始化回调（可为空）
    /// @param name 线程名（便于诊断）
    explicit EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                             std::string name = "IO-LoopThread");

    ~EventLoopThread();

    /// @brief 启动线程并等待 EventLoop 就绪
    /// @return 就绪的 EventLoop 指针（存活至本对象析构）
    /// @attention 仅可调用一次
    EventLoop* StartLoop();

 private:
    /// @brief 线程函数：创建 EventLoop → init 回调 → Loop() → 放行后销毁
    void threadFunc();

    /// @brief loop 初始化回调（loop 线程执行）
    ThreadInitCallback m_init_cb;
    /// @brief IO 线程（构造时注册 threadFunc，StartLoop 时才 run）
    Thread m_thread;
    /// @brief 是否已 StartLoop（仅宿主线程读写）
    bool m_started{false};
    /// @brief loop 指针（loop 线程发布、宿主线程读）
    std::atomic<EventLoop*> m_loop{nullptr};
    /// @brief loop 就绪通知（StartLoop 同步等待）
    std::promise<EventLoop*> m_ready;
    /// @brief Loop() 已返回通知
    std::promise<void> m_stopped;
    /// @brief m_stopped 的 future（宿主线程等待）
    std::future<void> m_stopped_future = m_stopped.get_future();
    /// @brief 宿主放行信号（此后线程方可析构 EventLoop）
    std::promise<void> m_may_finish;
};

//***********************************************
// name      : EventLoopThreadPool
// brief     : IO 线程池：管理 N 个 EventLoopThread，round-robin 分发连接
// attention : Start 幂等；getNextLoop 线程安全（0 线程退化为 base loop）；
//             析构自动停止并回收全部 IO 线程；setThreadNum 须在 Start 前
//***********************************************
class EventLoopThreadPool : noncopyable
{
 public:
    using ThreadInitCallback = EventLoopThread::ThreadInitCallback;

    /// @brief 构造 IO 线程池（不启动线程）
    /// @param baseLoop 主 loop（acceptor 所在；0 线程时兼作 IO loop）
    /// @param name 线程名前缀（实际线程名为 <name>-<i>）
    explicit EventLoopThreadPool(EventLoop* baseLoop, std::string name = "IO-Loop");

    /// @brief 析构自动停止并 join 全部 IO 线程
    ~EventLoopThreadPool() = default;

    /// @brief 设置 IO 线程数（默认 0：单 loop 模式）
    /// @attention 须在 Start 之前调用
    void setThreadNum(uint32_t num) noexcept
    {
        assert(!m_started.load());
        m_thread_num = num;
    }

    /// @brief 启动全部 IO 线程（幂等；重复调用无效果）
    /// @param cb 各 IO 线程的初始化回调
    void Start(const ThreadInitCallback& cb = ThreadInitCallback());

    /// @brief round-robin 取下一个 IO loop（线程安全）
    /// @return 无 IO 线程时返回 base loop
    ATTR_NODISCARD
    EventLoop* getNextLoop() noexcept;

    /// @brief 主 loop
    ATTR_PURE_INLINE
    EventLoop* getBaseLoop() const noexcept
    {
        return m_base_loop;
    }

    /// @brief 是否已启动
    ATTR_PURE_INLINE
    bool isStarted() const noexcept
    {
        return m_started.load();
    }

    /// @brief 线程名前缀
    ATTR_PURE_INLINE
    const std::string& name() const noexcept
    {
        return m_name;
    }

 private:
    /// @brief 主 loop（acceptor 所在；0 线程时兼作 IO loop）
    EventLoop* m_base_loop;
    /// @brief 线程名前缀
    std::string m_name;
    /// @brief 是否已启动
    std::atomic<bool> m_started{false};
    /// @brief IO 线程数
    uint32_t m_thread_num{0};
    /// @brief round-robin 游标
    std::atomic<uint32_t> m_next{0};
    /// @brief IO 线程宿主
    std::vector<std::unique_ptr<EventLoopThread>> m_threads;
    /// @brief 就绪的 IO loop 列表
    std::vector<EventLoop*> m_loops;
};

} // namespace bus
