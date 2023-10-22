#include <thread/ThreadPool.h>
#include <unistd.h>


int main()
{
    std::atomic<int> i;
    ThreadPool pool("ThreadPool");
    pool.start();
    while(i<1000)
    {
        pool.run([i=++i](){
            printf("%d\n",i);
        });
    }
    sleep(100);
    pool.stop();
    return 0;
}