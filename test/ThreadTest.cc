#include <thread/Thread.h>
#include <unistd.h>
#include <util/Util.h>

#include <iostream>

using namespace bus;

void func() {
  // while(1)
  {
    std::cout << CurrentThread::ThreadName() << CurrentThread::tid() << ":"
              << CurrentThread::isMainThread() << "," << CurrentThread::getCPU()
              << std::endl;
    // sleep(10);
  }
}

int main() {
  Thread t(func, "Test");
  t.run();
  t.bindCPU(11);
  sleep(5);
  std::cout << t.getThreadName() << t.getTid() << std::endl;
  std::cout << CurrentThread::ThreadName() << CurrentThread::tid() << ":"
            << CurrentThread::isMainThread() << "," << CurrentThread::getCPU()
            << std::endl;
  t.join();

  return 0;
}