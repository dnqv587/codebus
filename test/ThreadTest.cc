#include <iostream>
#include <unistd.h>
#include <thread/Thread.h>

void func()
{
    //while(1)
    {
        std::cout<<CurrentThread::ThreadName()<<CurrentThread::tid()<<":"<<CurrentThread::isMainThread()<<std::endl;
        //sleep(10);
    }

}

int main()
{

    Thread t(func, "Test");
    t.run();
    sleep(5);
    std::cout<<t.getThreadName()<<t.getTid()<<std::endl;
    std::cout<<CurrentThread::ThreadName()<<CurrentThread::tid()<<":"<<CurrentThread::isMainThread()<<std::endl;
    t.join();

    return 0;
}