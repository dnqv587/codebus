#include <thread/CurrentThread.h>
#include <base/Macro.h>
#include <sys/syscall.h>
#include <unistd.h>

thread_local pid_t CurrentThread::t_threadId = 0;//pid起始值为1
thread_local const char* CurrentThread::t_threadName="UnnamedThread";

namespace
{
inline pid_t gettid()
{
    return static_cast<pid_t>(::syscall(SYS_gettid));
}
}

pid_t CurrentThread::tid()
{
    if(UNLIKELY(0 == t_threadId))
    {
        t_threadId = ::gettid();
    }
    return t_threadId;
}

const char *CurrentThread::ThreadName()
{
    return t_threadName;
}

bool CurrentThread::isMainThread()
{
    //主线程tid等于进程的pid
    return tid() == ::getpid();
}
