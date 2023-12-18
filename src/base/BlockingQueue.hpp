#pragma once
#include <base/noncopyable.hpp>
#include <thread/Mutex.hpp>
#include <thread/Condition.hpp>
#include <base/Macro.h>
#include <queue>
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread/CurrentThread.h>


//***********************************************
// class     : BlockingQueue
// brief     : 无界阻塞队列
// attention : None
// date      : 2023/10/19
//***********************************************
template<typename T>
class BlockingQueue:noncopyable
{
public:
    using value_type=T;
    using size_type=size_t;
    BlockingQueue();
    ~BlockingQueue();
    /// @brief 入队
    void put(const T& val);
    /// @brief 入队
    void put(T&& val);
    /// @brief 出队，容器为空阻塞
    T take();
    /// @brief 清空并返回队列
    std::queue<T> drain();

    /// @brief 是否为空
    bool isEmpty();
    /// @brief 容量
    size_t size();
    /// @brief 解除所有阻塞
    void over();

private:
    std::queue<T> m_queue;
    MutexLock m_mutex;
    Condition m_cond;
    std::atomic<bool> m_isRunning;
};

//***********************************************
// class     : BoundBlockingQueue
// brief     : 有界阻塞队列
// attention : None
// date      : 2023/10/19
//***********************************************
template<typename T>
class BoundBlockingQueue:noncopyable
{
public:
    using value_type=T;
    using size_type=size_t;
    BoundBlockingQueue(int maxNum);
    ~BoundBlockingQueue();
    /// @brief 入队,容器未满则阻塞
    void put(const T& val);
    /// @brief 入队，容器未满则阻塞
    void put(T&& val);
    /// @brief 出队，容器为空阻塞
    T take();
    /// @brief 清空并返回队列
    std::queue<T> drain();
    /// @brief 是否为空
    bool isEmpty();
    /// @brief 是否为满
    bool isFull();
    /// @brief 容量
    size_t size();
    /// @brief 解除所有阻塞
    void over();

private:
    std::queue<T> m_queue;
    MutexLock m_mutex;
    Condition m_waitNotEmpty;
    Condition m_waitNotFull;
    std::atomic<bool> m_isRunning;
    std::atomic<int> m_maxSize;
};



template<typename T>
BoundBlockingQueue<T>::BoundBlockingQueue(int maxNum)
: m_waitNotEmpty(m_mutex),m_waitNotFull(m_mutex), m_isRunning(true), m_maxSize(maxNum)
{
    //assert(m_maxSize>0);
    if(UNLIKELY(m_maxSize>0))
    {
        throw std::exception();
    }
}

template<typename T>
BoundBlockingQueue<T>::~BoundBlockingQueue()
{
    m_waitNotEmpty.notifyAll();
}

template<typename T>
std::queue<T> BoundBlockingQueue<T>::drain()
{
    std::queue<T> que;
    {
        MutexLockGuard lock(m_mutex);
        std::move(que,m_queue);
        assert(que.empty());
    }
    return que;
}

template<typename T>
void BoundBlockingQueue<T>::over()
{
    m_isRunning=false;
    m_waitNotEmpty.notifyAll();
    m_waitNotFull.notifyAll();
}

template<typename T>
size_t BoundBlockingQueue<T>::size()
{
    MutexLockGuard lock(m_mutex);
    return m_queue.size();
}

template<typename T>
bool BoundBlockingQueue<T>::isFull()
{
    MutexLockGuard lock(m_mutex);
    return m_queue.size()>=m_maxSize;
}

template<typename T>
bool BoundBlockingQueue<T>::isEmpty()
{
    MutexLockGuard lock(m_mutex);
    return m_queue.empty();
}

template<typename T>
T BoundBlockingQueue<T>::take()
{
    MutexLockGuard lock(m_mutex);
    while(UNLIKELY(m_isRunning && m_queue.empty()))
    {
        m_waitNotEmpty.wait();
    }
    if(UNLIKELY(m_queue.empty()))
    {
        return T();
    }
    T&& front(std::move(m_queue.front()));
    m_queue.pop();
    m_waitNotFull.notify();
    return front;
}

template<typename T>
void BoundBlockingQueue<T>::put(T &&val)
{
    MutexLockGuard lock(m_mutex);
    while(m_isRunning && m_queue.size()>=m_maxSize)
    {
        m_waitNotFull.wait();
    }
    m_queue.emplace(std::move(val));
    m_waitNotEmpty.notify();
}

template<typename T>
void BoundBlockingQueue<T>::put(const T &val)
{
    MutexLockGuard lock(m_mutex);
    while(m_isRunning && m_queue.size()>=m_maxSize)
    {
        m_waitNotFull.wait();
    }
    m_queue.push(val);
    m_waitNotEmpty.notify();
}

template<typename T>
BlockingQueue<T>::BlockingQueue():m_cond(m_mutex),m_isRunning(true){}

template<typename T>
void BlockingQueue<T>::over()
{
    m_isRunning=false;
    m_cond.notifyAll();
}

template<typename T>
size_t BlockingQueue<T>::size()
{
    MutexLockGuard lock(m_mutex);
    return m_queue.size();
}

template<typename T>
bool BlockingQueue<T>::isEmpty()
{
    MutexLockGuard lock(m_mutex);
    return m_queue.empty();
}

template<typename T>
std::queue<T> BlockingQueue<T>::drain()
{
    std::queue<T> que;
    {
        MutexLockGuard lock(m_mutex);
        std::swap(que,m_queue);
        assert(m_queue.empty());
    }
    return que;
}

template<typename T>
T BlockingQueue<T>::take()
{
    MutexLockGuard lock(m_mutex);
    while(UNLIKELY(m_isRunning && m_queue.empty()))
    {
        m_cond.wait();
    }
    if(UNLIKELY(m_queue.empty()))
    {
        return T();
    }
    T&& front(std::move(m_queue.front()));
    m_queue.pop();
    return front;
}

template<typename T>
void BlockingQueue<T>::put(T &&val)
{
    MutexLockGuard lock(m_mutex);
    m_queue.emplace(std::move(val));
    m_cond.notify();
}

template<typename T>
void BlockingQueue<T>::put(const T &val)
{
    MutexLockGuard lock(m_mutex);
    m_queue.push(val);
    m_cond.notify();
}

template<typename T>
BlockingQueue<T>::~BlockingQueue()
{
    m_cond.notifyAll();
}
