#include <iostream>
#include <thread/Condition.hpp>
#include <unistd.h>
MutexLock mutex;

void* Func(void* arg)
{
    {
        MutexLockGuard lock(mutex);
        std::cout << "线程" << std::endl;
        Condition *cond = static_cast<Condition *>(arg);
        cond->notify();
        sleep(5);
    }
    sleep(10);
    std::cout<<"线程结束"<<std::endl;
    return nullptr;
}

int main()
{

    Condition cond(mutex);
    ::pthread_t thread;
    {
        MutexLockGuard lock(mutex);
        ::pthread_create(&thread, nullptr, Func, &cond);

        std::cout << "等待" << std::endl;
        sleep(1);
        std::cout << "1秒" << std::endl;
        cond.TimeWait(100);
    }
    std::cout<<"等待结束"<<std::endl;
    ::pthread_join(thread, nullptr);

    return 0;
}