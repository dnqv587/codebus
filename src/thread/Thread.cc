#include <thread/Thread.h>
#include <assert.h>
#include <stdexcept>
#include <memory>

Thread::Thread(Thread::ThreadFunc func, const std::string &threadName)
:m_tid(0),m_pthreadId(0),m_func(func),m_threadName(threadName),m_latch(1)
{

}

void Thread::run()
{

    if (::pthread_create(&m_pthreadId, nullptr, threadFunc, this))
    {
        throw std::logic_error("pthread_create error");
    }
    m_latch.wait();

}

void Thread::join()
{
    assert(m_tid);
    if(::pthread_join(m_pthreadId, nullptr))
    {
        throw std::logic_error("pthread_join error");
    }
    m_tid=0;
}

Thread::~Thread()
{
    if(!m_tid)
    {
        ::pthread_detach(m_pthreadId);
    }
}

void *Thread::threadFunc(void *arg)
{
    Thread* t=static_cast<Thread*>(arg);
    t->m_tid=CurrentThread::tid();
    CurrentThread::setThreadName(t->m_threadName.c_str());
    t->m_pthreadId=::pthread_self();
    ThreadFunc func;
    func.swap(t->m_func);

    t->m_latch.coutDown();
    //设置线程名
    ::pthread_setname_np(::pthread_self(),CurrentThread::ThreadName());

    func();

    return nullptr;

}
