# 线程模块（thread）

## 功能概述

线程模块封装 pthread 核心能力：线程（`Thread`）、线程池（`ThreadPool`）、互斥锁（`MutexLock`）、读写锁（`RWLock`）、条件变量（`Condition`）、信号量（`Semaphore`）、倒计时门闩（`CountDownLatch`），以及线程本地信息（`CurrentThread`）与进程信息（`ProcessInfo`）。

## 设计原理

- 所有同步原语均为 pthread 的 RAII 薄封装，构造初始化、析构销毁，不可复制（继承 `noncopyable`）
- `MutexLockGuard` / `ReadLockGuard` / `WriteLockGuard` 提供作用域自动加解锁
- `Thread::run()` 通过 `CountDownLatch` 保证线程函数真正启动后才返回（避免竞态）
- `ThreadPool` 采用固定线程数 + 无界 `BlockingQueue<Task>` 的任务队列模型

## 使用示例

```cpp
#include "thread/Thread.h"
#include "thread/ThreadPool.h"

using namespace bus;

// 单个线程（可绑定 CPU 核心）
Thread t([] {
    // 任务
}, "worker");
t.run();
t.bindCPU(0);     // 绑定 0 号核心
t.join();

// 线程池
ThreadPool pool("worker-pool");
pool.start(4);    // 4 个工作线程；缺省为 CPU 核心数
pool.run([] { /* 任务 1 */ });
pool.run([] { /* 任务 2 */ });
pool.stop();
```

### 同步原语

```cpp
#include "thread/Mutex.hpp"
#include "thread/Condition.hpp"

MutexLock mutex;
{
    MutexLockGuard lock(mutex);   // 自动加锁
    // 临界区
}                                 // 自动解锁

Condition cond(mutex);            // 绑定同一把锁
cond.wait();                      // 阻塞等待
cond.notify();                    // 唤醒一个
cond.notifyAll();                 // 唤醒全部
```

## 接口说明

### Thread

| 接口 | 说明 |
|---|---|
| `Thread(func, name)` | 构造；name 默认 "UnnamedThread" |
| `run()` | 启动线程（等待线程真正启动后返回） |
| `join()` | 等待线程结束 |
| `bindCPU(cpuid)` | 绑定 CPU 亲和性（pthread_setaffinity_np） |
| `getTid()` | 线程 ID（pid_t） |
| `getThreadName()` | 线程名 |

### ThreadPool

| 接口 | 说明 |
|---|---|
| `ThreadPool(name)` | 构造 |
| `start(threadNum)` | 启动，threadNum 默认 CPU 核心数 |
| `run(task)` | 提交任务 `std::function<void()>` |
| `stop()` | 停止并回收所有线程 |

### 同步原语

| 类 | 接口 | 说明 |
|---|---|---|
| `MutexLock` | `lock()` / `unlock()` / `getNativeHandle()` | 互斥锁 |
| `MutexLockGuard` | RAII 构造/析构 | 互斥锁守卫 |
| `RWLock` | `rdlock()` / `wrlock()` / `unlock()` | 读写锁 |
| `ReadLockGuard` / `WriteLockGuard` | RAII | 读/写锁守卫 |
| `Condition` | `wait()` / `TimeWait(seconds)` / `notify()` / `notifyAll()` | 条件变量 |
| `Semaphore` | `wait()` / `notify()` | POSIX 信号量 |
| `CountDownLatch` | `wait()` / `coutDown()` / `getCount()` | 倒计时门闩 |

### CurrentThread / ProcessInfo

| 接口 | 说明 |
|---|---|
| `CurrentThread::tid()` | 当前线程 ID（线程本地缓存） |
| `CurrentThread::ThreadName()` | 当前线程名 |
| `CurrentThread::isMainThread()` | 是否主线程 |
| `CurrentThread::getCPU()` | 当前线程绑定的 CPU |
| `ProcessInfo::Pid()` | 进程 ID |
| `ProcessInfo::hostname()` | 主机名 |

## 注意事项

- `MutexLock`、`RWLock` 不可复制；条件变量必须与互斥锁配对使用
- `MutexLockGuard` 系列宏防御（同名宏 `static_assert`）用于防止写出 `MutexLockGuard(mutex);` 这类产生匿名临时对象、立即解锁的错误代码——守卫必须命名
- `Condition::TimeWait(seconds)` 的返回值语义与注释相反（当前实现超时时返回 `true`），使用时需注意
- `CountDownLatch::coutDown()` 方法名为历史拼写（应为 countDown），请勿修改调用方假设其正确拼写
- `Thread` 析构前必须 `join()` 或已结束，否则线程资源泄漏
- 线程池 `stop()` 后不可再 `run()` 提交任务
