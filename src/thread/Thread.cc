#include <thread/Thread.h>
#include "util/Util.h"
#include <assert.h>
#include <stdexcept>
#include <memory>
#include <utility>
#include <cstring>

namespace bus
{
Thread::Thread(Thread::ThreadFunc func, std::string threadName)
	: m_tid(0), m_pthreadId(0), m_func(std::move(func)), m_threadName(std::move(threadName)), m_latch(1)
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

void Thread::join() {
  assert(m_tid);
  if (::pthread_join(m_pthreadId, nullptr)) {
    throw std::logic_error("pthread_join error");
  }
  m_tid = 0;
}
void Thread::bindCPU(const uint32_t cpuid) {
  cpu_set_t cpuset{};
  CPU_ZERO(&cpuset);        // 清空集合
  CPU_SET(cpuid, &cpuset);  // 添加目标核心到集合

  assert(cpuid < SystemUtil::getCoreNum());

  // 设置线程的CPU亲和性
  const int rc =
      pthread_setaffinity_np(m_pthreadId, sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    throw std::runtime_error(strerror(rc));
  }
}

Thread::~Thread()
{
	if (!m_tid)
	{
		::pthread_detach(m_pthreadId);
	}
}

void* Thread::threadFunc(void* arg)
{
	auto* t = static_cast<Thread*>(arg);
	t->m_tid = CurrentThread::tid();
	CurrentThread::setThreadName(t->m_threadName.c_str());
	t->m_pthreadId = ::pthread_self();
	ThreadFunc func;
	func.swap(t->m_func);

	t->m_latch.coutDown();
	//设置线程名
	::pthread_setname_np(::pthread_self(), CurrentThread::ThreadName());

	func();

	return nullptr;

}
}