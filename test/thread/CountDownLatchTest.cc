//***********************************************
// name      : CountDownLatchTest.cc
// brief     : thread 模块单元测试：被测 src/thread/CountDownLatch.hpp（倒计时门闩）
// attention : 倒数方法按源码现名 coutDown（拼写见 Q8）；改名后同步更新用例
//***********************************************
#include <gtest/gtest.h>
#include "thread/CountDownLatch.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace bus {
namespace test {

// ---------------------------------------------------------------------------
// 迁移用例（自旧 ThreadTest）
// ---------------------------------------------------------------------------

TEST(CountDownLatchTest, CountDownLatchSync) {
    constexpr int kThreads = 4;
    CountDownLatch latch(kThreads);

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&latch]() { latch.coutDown(); });  // TODO(Q8): 现名 coutDown
    }
    latch.wait();
    EXPECT_EQ(latch.getCount(), 0);

    for (auto& thread : threads) {
        thread.join();
    }
}

// ---------------------------------------------------------------------------
// 新增场景（P8）
// ---------------------------------------------------------------------------

/// [界] count=0 构造后 wait() 直通不阻塞
TEST(CountDownLatchTest, ZeroCountWaitReturnsImmediately) {
    CountDownLatch latch(0);
    std::atomic<bool> passed{false};

    std::thread waiter([&]() {
        latch.wait();
        passed.store(true);
    });
    waiter.join();
    EXPECT_TRUE(passed.load());
    EXPECT_EQ(latch.getCount(), 0);
}

/// [并] 8 waiter 等待 8 次 coutDown（多方同闸）
TEST(CountDownLatchTest, EightWaitersEightCounts) {
    constexpr int kThreads = 8;
    CountDownLatch latch(kThreads);
    std::atomic<int> released{0};

    std::vector<std::thread> waiters;
    for (int i = 0; i < kThreads; ++i) {
        waiters.emplace_back([&]() {
            latch.wait();
            released.fetch_add(1);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(released.load(), 0);  // 未倒数完成前无人通过

    for (int i = 0; i < kThreads; ++i) {
        latch.coutDown();
    }
    for (auto& thread : waiters) {
        thread.join();
    }
    EXPECT_EQ(released.load(), kThreads);
}

/// [正] getCount 随倒数递减可观测
TEST(CountDownLatchTest, CountDecrements) {
    CountDownLatch latch(3);
    EXPECT_EQ(latch.getCount(), 3);

    latch.coutDown();
    EXPECT_EQ(latch.getCount(), 2);

    latch.coutDown();
    latch.coutDown();
    EXPECT_EQ(latch.getCount(), 0);
}

}  // namespace test
}  // namespace bus
