#pragma once
#include <base/noncopyable.hpp>
#include <thread/CurrentThread.h>
#include <thread/CountDownLatch.hpp>
#include <memory>
#include <functional>
#include <string>

class Thread:CurrentThread,std::enable_shared_from_this<Thread>
{
public:
    using ptr=std::shared_ptr<Thread>;
    using ThreadFunc=std::function<void ()>;

    explicit Thread(ThreadFunc func,const std::string& threadName="UnnamedThread");
    ~Thread();
    /// @brief 启动线程
    void run();

    void join();

    pid_t getTid() const
    {return m_tid;}

    std::string getThreadName() const
    {return m_threadName;}

private:
    static void* threadFunc(void* arg);

    /// @brief 线程id
    pid_t m_tid;
    /// @brief 线程
    pthread_t m_pthreadId;
    /// @brief 线程函数
    ThreadFunc m_func;
    /// @brief 线程名
    std::string m_threadName;
    /// @brief 倒计同步
    CountDownLatch m_latch;
};