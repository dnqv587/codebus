# 定时器模块（Timer）

## 功能概述

定时器模块基于 `timerfd` 实现毫秒级定时任务调度：支持**一次性**与**周期**定时器，由 `TimerManager` 统一管理，挂载在所属 `EventLoop` 上，超时回调在 IO 线程内执行。

## 设计原理

- **timerfd**：`TimerManager` 持有一个 `timerfd` 并注册为 `Channel`，设置到期时间后阻塞在 `epoll_wait` 的 EventLoop 会被定时触发，无需额外线程
- **有序集合**：定时器按到期时间存入 `std::set<Timer, Comparator>`（比较器按 `Expiration()` 排序，到期时间相同再比较指针保证唯一性）
- **哨兵模式**：`m_sentry` 哨兵定时器用于"处理到期定时器期间新添加的定时器"（如果新定时器比当前更早到期，通过哨兵触发二次 `timerfd_settime`）
- **取消语义**：`Timer::cancel()` 只做标记（`std::atomic<bool>`），实际移除由 Manager 在到期处理时惰性完成，避免加锁
- **循环定时器**：到期后自动按 `m_repeatTime` 重新计算下一次到期时间并重新插入集合

## 使用示例

```cpp
#include "net/EventLoop.h"
#include "Timer.h"

using namespace bus;
using namespace std::chrono;

int main() {
    EventLoop loop;
    TimerManager timers(std::ref(loop));

    // 1 秒后触发一次（interval 为 0）
    timers.addTimer([] { /* 一次性任务 */ },
                    steady_clock::now() + 1s, milliseconds(0));

    // 每 500ms 周期触发
    auto timer = timers.addTimer([] { /* 周期任务 */ },
                                 steady_clock::now() + 500ms,
                                 milliseconds(500));

    // 取消定时器（惰性生效）
    timer->cancel();

    // 调整周期定时器：立即到期并重新计时
    timer->refresh();

    loop.Loop();   // 进入事件循环后定时器生效
    return 0;
}
```

## 接口说明

### TimerManager

| 接口 | 说明 |
|---|---|
| `TimerManager(loop)` | 构造，绑定所属 `EventLoop`（`std::reference_wrapper`） |
| `addTimer(cb, when, interval)` | 添加定时器；`when` 为到期时间点（`steady_clock` 纳秒精度），`interval` 为循环周期（毫秒，0 = 一次性） |
| `~TimerManager()` | 析构自动关闭 timerfd 与清理 |

### Timer

| 接口 | 说明 |
|---|---|
| `refresh()` | 立即到期并按周期重新计时 |
| `cancel()` | 标记取消（到期时不再触发，惰性移除） |
| `Expiration()` | 到期时间（`std::chrono::nanoseconds`） |
| `getRepeatTime()` | 循环周期（毫秒） |
| `isRepeat()` | 是否周期定时器 |
| `isCanceled()` | 是否已取消 |

## 注意事项

- **线程模型**：定时器回调在所属 EventLoop 的 IO 线程中执行，回调内请勿做长时间阻塞操作；如需跨线程操作定时器，须通过 `EventLoop::RunInLoop()` 投递（Manager 本身未加锁）
- 到期时间基于 `std::chrono::steady_clock`（单调时钟），不受系统时间调整影响
- `Timer` 生命周期：Manager 持有到期前的定时器，取消后由 Manager 在到期处理时释放；使用方如持有 `Timer::ptr` 需注意不要在回调中销毁自身造成悬垂
- 定时精度受事件循环繁忙程度影响，不适用于硬实时场景
