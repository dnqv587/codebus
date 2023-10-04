#pragma once
#include <thread/Mutex.hpp>
#include <memory>
#include <vector>

//***********************************************
// name      : Observer.hpp
// brief     : 观察者模式
// attention : None
// date      : 2023/9/19
//***********************************************
class Observable;

/// @brief 观察着类
class Observer:public std::enable_shared_from_this<Observer>
{
public:
    virtual ~Observer(){};
    /// @brief 触发事件
    virtual void Update()=0;
    /// @brief 注册观察
    /// @param observable 观察对象
    void Observe(std::shared_ptr<Observable> observable);
private:
    std::weak_ptr<Observable> m_observable;
};

class Observable
{
public:
    /// @brief 通知事件
    void Notify();
    //注册被观察
    void Register(std::weak_ptr<Observer> observer);
private:
    std::vector<std::weak_ptr<Observer>> m_ObserverArr;
    MutexLock m_mutex;
};

void Observer::Observe(std::shared_ptr<Observable> observable)
{
    observable->Register(std::weak_ptr<Observer>(shared_from_this()));
    m_observable=std::weak_ptr<Observable>(observable);
}

void Observable::Register(std::weak_ptr<Observer> observer)
{
    m_ObserverArr.push_back(observer);
}

void Observable::Notify()
{
    MutexLockGuard lock(m_mutex);
    auto iter=m_ObserverArr.begin();
    while(iter!=m_ObserverArr.end())
    {
        auto obj=iter->lock();
        if(obj)
        {
            obj->Update();
            ++iter;
        }
        else
        {
            iter=m_ObserverArr.erase(iter);
        }
    }
}