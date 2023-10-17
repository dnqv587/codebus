#pragma once
#include <base/noncopyable.hpp>
#include <semaphore.h>
#include <stdexcept>

class Semaphore:noncopyable
{
public:
    Semaphore(int count=0)
    {
        if(::sem_init(&m_sem,0,count))
        {
            throw std::logic_error("sem_init error");
        }
    }

    ~Semaphore() noexcept
    {::sem_destroy(&m_sem);}

    void wait()
    {
        if(::sem_wait(&m_sem))
        {
            throw std::logic_error("sem_wait error");
        }
    }

    void notify()
    {
        if(::sem_post(&m_sem))
        {
            throw std::logic_error("sem_post error");
        }
    }

private:
    sem_t m_sem;
};