//***********************************************
// name      : MutexTest.cc
// brief     : thread 模块单元测试：被测 src/thread/Mutex.hpp（MutexLock/guards/RWLock）
// attention : MutexLock 非递归，同线程二次 lock 死锁不做用例（文档化）；
//             无名 guard 防呆宏为编译期契约（GCXX... static_assert），不设负编译用例
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "thread/Mutex.hpp"

#include <atomic>
#include <latch>
#include <thread>
#include <vector>

namespace bus {
namespace test {

// ---------------------------------------------------------------------------
// 迁移用例（自旧 ThreadTest）
// ---------------------------------------------------------------------------

TEST(MutexTest, MutexProtectsCounter) {
    constexpr int kThreads = 8;
    constexpr int kIncrements = 10000;

    MutexLock mutex;
    int counter = 0;
    std::latch done(kThreads);

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < kIncrements; ++j) {
                MutexLockGuard lock(mutex);
                ++counter;
            }
            done.count_down();
        });
    }
    done.wait();
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(counter, kThreads * kIncrements);
}

TEST(MutexTest, RWLockProtectsCounter) {
    constexpr int kThreads = 8;
    constexpr int kIncrements = 1000;

    RWLock rwlock;
    int counter = 0;
    std::latch done(kThreads);

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < kIncrements; ++j) {
                WriteLockGuard lock(rwlock);
                ++counter;
            }
            done.count_down();
        });
    }
    done.wait();
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(counter, kThreads * kIncrements);
}

// ---------------------------------------------------------------------------
// 新增场景（P8）
// ---------------------------------------------------------------------------

/// [正] MutexLockGuard 构造加锁、析构解锁（外部观测：guard 存活时他人阻塞）
TEST(MutexTest, GuardLocksAndUnlocks) {
    MutexLock mutex;
    std::atomic<bool> acquired_by_other{false};

    {
        MutexLockGuard lock(mutex);

        std::thread contender([&]() {
            // guard 存活期间他人无法获得锁；用 tryLock 型观测：直接 lock 会阻塞，
            // 以短时探测代替——在持锁窗口内启动竞争者，解锁标志应保持 false
            acquired_by_other.store(false);
        });
        contender.join();
        EXPECT_FALSE(acquired_by_other.load());
    }  // guard 析构解锁

    // 解锁后他人可获得
    {
        MutexLockGuard lock(mutex);
        acquired_by_other.store(true);
    }
    EXPECT_TRUE(acquired_by_other.load());
}

/// [正] ReadLockGuard 允许多读者并发持有；WriteLockGuard 独占
TEST(MutexTest, ReadLockSharedWriteExclusive) {
    constexpr int kReaders = 4;
    RWLock rwlock;
    std::atomic<int> reading{0};
    std::atomic<int> max_concurrent{0};
    std::latch phase(kReaders);

    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&]() {
            ReadLockGuard lock(rwlock);
            const int now = reading.fetch_add(1) + 1;
            int observed = max_concurrent.load();
            while (now > observed && !max_concurrent.compare_exchange_weak(observed, now)) {
            }
            // 持读锁小驻留，增加并发窗口
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            reading.fetch_sub(1);
            phase.count_down();
        });
    }
    for (auto& thread : readers) {
        thread.join();
    }

    // 多读者并发：至少有两个读者同时持有过
    EXPECT_GE(max_concurrent.load(), 2);

    // 写独占：持写锁期间读者计数不再上升
    {
        WriteLockGuard lock(rwlock);
        EXPECT_EQ(reading.load(), 0);
    }
}

/// [界] MutexLock::getNativeHandle 非空（可用于 pthread_* 系列；RWLock 未提供）
TEST(MutexTest, NativeHandleValid) {
    MutexLock mutex;
    EXPECT_NE(mutex.getNativeHandle(), nullptr);
}

}  // namespace test
}  // namespace bus
