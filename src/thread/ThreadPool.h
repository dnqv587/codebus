#pragma once
#include <base/noncopyable.hpp>
#include <base/BlockingQueue.hpp>
#include <thread/Thread.h>
#include <functional>
#include <vector>
#include <atomic>

//***********************************************
// class     : ThreadPool
// brief     : 线程池
// attention : None
// date      : 2023/10/19
//***********************************************
class ThreadPool:noncopyable
{
public:
    using ptr=std::shared_ptr<ThreadPool>;
    using Task=std::function<void ()>;


    explicit ThreadPool(std::string name);
    ~ThreadPool();
    /// @brief 启动线程池
    /// @param threadNum 线程数
    void start(int threadNum=CORE_NUM);
    /// @brief 停止线程池
    void stop();
    /// @brief 分配运行任务给线程吃
    /// @param task CallBack,void (*)()
    void run(Task task);

private:
    /// @brief 核心数
    static const int CORE_NUM;
    void threadFunc();

    /// @brief 任务队列
    BoundBlockingQueue<Task> m_taskQueue;
    /// @brief 线程容器
    std::vector<Thread::ptr> m_threads;
    /// @brief  线程池是否运行
    std::atomic<bool> m_isRunning;
    /// @brief 线程池名
    const std::string m_name;
};