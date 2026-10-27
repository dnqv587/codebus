//***********************************************
// name      : ConditionTest.cc
// brief     : thread 模块单元测试：被测 src/thread/Condition.hpp（条件变量）
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "thread/Condition.hpp"
#include "thread/Mutex.hpp"

#include <chrono>
#include <thread>
#include <vector>

namespace bus {
namespace test {

// ---------------------------------------------------------------------------
// 迁移用例（自旧 ThreadTest）
// ---------------------------------------------------------------------------

TEST(ConditionTest, ConditionNotify) {
    MutexLock mutex;
    Condition condition(mutex);
    bool ready = false;

    std::thread producer([&]() {
        MutexLockGuard lock(mutex);
        ready = true;
        condition.notify();
    });

    {
        MutexLockGuard lock(mutex);
        while (!ready) {
            condition.wait();
        }
    }
    producer.join();
    EXPECT_TRUE(ready);
}

/// [错] 超时路径：TimeWait 超时返回 false
TEST(ConditionTest, TimeWaitReturnsFalseOnTimeout) {
    MutexLock mutex;
    Condition condition(mutex);
    MutexLockGuard lock(mutex);
    EXPECT_FALSE(condition.TimeWait(1));
}

// ---------------------------------------------------------------------------
// 新增场景（P8）
// ---------------------------------------------------------------------------

/// [正] notifyAll 唤醒全部 N 个等待者（notify 单播的补集）
TEST(ConditionTest, NotifyAllWakesAllWaiters) {
    constexpr int kWaiters = 4;
    MutexLock mutex;
    Condition condition(mutex);
    int ready = 0;
    int woken = 0;

    std::vector<std::thread> waiters;
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&]() {
            MutexLockGuard lock(mutex);
            ++ready;
            while (woken == 0) {
                condition.wait();
            }
            ++woken;
        });
    }

    // 等全部进入等待
    ASSERT_TRUE(WaitFor([&]() {
        MutexLockGuard lock(mutex);
        return ready == kWaiters;
    }));

    {
        MutexLockGuard lock(mutex);
        woken = 1;  // 广播标记
        condition.notifyAll();
    }

    for (auto& thread : waiters) {
        thread.join();
    }
    EXPECT_EQ(woken, 1 + kWaiters);
}

/// [正] TimeWait 在超时窗口内被 notify 唤醒返回 true
TEST(ConditionTest, TimeWaitWokenByNotify) {
    MutexLock mutex;
    Condition condition(mutex);

    std::thread notifier([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        MutexLockGuard lock(mutex);
        condition.notify();
    });

    MutexLockGuard lock(mutex);
    const bool result = condition.TimeWait(2000);
    notifier.join();
    EXPECT_TRUE(result);
}

}  // namespace test
}  // namespace bus
