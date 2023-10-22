#include <thread/ThreadPool.h>
#include <unistd.h>
#include <assert.h>

const int ThreadPool::CORE_NUM=static_cast<int>(::sysconf(_SC_NPROCESSORS_CONF));

ThreadPool::ThreadPool(std::string name)
:m_isRunning(false),
 m_taskQueue(10),
m_name(std::move(name))
{
}

ThreadPool::~ThreadPool()
{
    if(m_isRunning)
    {
        stop();
    }
}

void ThreadPool::start(int threadNum)
{
    assert(m_threads.empty() && !m_isRunning);
    m_isRunning=true;
    m_threads.reserve(threadNum);
    for (int i=0;i<threadNum;++i)
    {
        m_threads.emplace_back(new Thread([this]()->void {
            this->threadFunc();
        },m_name+"["+std::to_string(i)+"]"));
        m_threads[i]->run();
    }

}

void ThreadPool::threadFunc()
{
    while(LIKELY(m_isRunning))
    {
        Task task = std::move(m_taskQueue.take());
        if(LIKELY(task))
        {
            task();
        }
    }
}

void ThreadPool::stop()
{
    m_isRunning=false;
    m_taskQueue.over();
    for(const Thread::ptr& thread : m_threads)
    {
        thread->join();
    }
}

void ThreadPool::run(ThreadPool::Task task)
{
    if(m_threads.empty())
    {
        task();
    }
    else if(m_isRunning)
    {
        m_taskQueue.put(std::move(task));
    }
}


