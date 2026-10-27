//***********************************************
// name      : SemaphoreTest.cc
// brief     : thread 模块单元测试：被测 src/thread/Semaphore.hpp（计数信号量）
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "thread/Semaphore.hpp"

#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

namespace bus {
namespace test {

// ---------------------------------------------------------------------------
// 迁移用例（自旧 ThreadTest）
// ---------------------------------------------------------------------------

TEST(SemaphoreTest, SemaphoreSync) {
    Semaphore semaphore(0);
    std::atomic<bool> signaled{false};

    std::thread waiter([&]() {
        semaphore.wait();
        signaled.store(true);
    });

    semaphore.notify();
    waiter.join();
    EXPECT_TRUE(signaled.load());
}

// ---------------------------------------------------------------------------
// 新增场景（P8）
// ---------------------------------------------------------------------------

/// [界] 初值 2：两个 waiter 并发通过，第三个阻塞直至再 notify
TEST(SemaphoreTest, InitialPermitsBoundConcurrency) {
    Semaphore semaphore(2);
    std::atomic<int> passed{0};
    std::latch two_passed(2);

    std::vector<std::thread> waiters;
    for (int i = 0; i < 3; ++i) {
        waiters.emplace_back([&]() {
            semaphore.wait();
            passed.fetch_add(1);
            if (passed.load() <= 2) {
                two_passed.count_down();
            }
        });
    }

    // 前两个应立即通过
    two_passed.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(passed.load(), 2);  // 第三个仍阻塞

    semaphore.notify();
    for (auto& thread : waiters) {
        thread.join();
    }
    EXPECT_EQ(passed.load(), 3);
}

/// [并] N 次 notify 与 N 个 waiter 一一放行（计数守恒，不多不少）
TEST(SemaphoreTest, NotifyWaiterConservation) {
    constexpr int kWaiters = 8;
    Semaphore semaphore(0);
    std::atomic<int> released{0};

    std::vector<std::thread> waiters;
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&]() {
            semaphore.wait();
            released.fetch_add(1);
        });
    }

    // 仅 notify kWaiters-1 次：恰有一个 waiter 仍阻塞
    for (int i = 0; i < kWaiters - 1; ++i) {
        semaphore.notify();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(released.load(), kWaiters - 1);

    semaphore.notify();  // 最后一次放行
    for (auto& thread : waiters) {
        thread.join();
    }
    EXPECT_EQ(released.load(), kWaiters);
}

}  // namespace test
}  // namespace bus
