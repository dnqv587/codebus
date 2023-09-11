#pragma once
#include <pthread.h>

class CurrentThread
{
public:
	int32_t tid() const
	{
		return t_threadId;
	}

private:
	thread_local static const char* t_threadName;
	thread_local static int t_threadId;
}
