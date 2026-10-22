//***********************************************
// name      : ThreadPoolTest.cc
// brief     : thread 模块单元测试：被测 src/thread/ThreadPool.h/.cc（线程池）
// attention : start() 二次调用与 worker 内任务抛异常均为现状锁定（assert /
//             进程终止，Q9）；含 EXPECT_EXIT 死亡测试，sanitizer 下守卫跳过
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "thread/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

// ---------------------------------------------------------------------------
// 迁移用例（自旧 ThreadTest）
// ---------------------------------------------------------------------------

TEST(ThreadPoolTest, ExecutesAllTasks)
{
    constexpr int kTasks = 1000;
    ThreadPool pool("test-pool");
    pool.start(4);

    std::latch done(kTasks);
    for (int i = 0; i < kTasks; ++i)
    {
        pool.run([&done]() { done.count_down(); });
    }
    done.wait();
    pool.stop();

    SUCCEED();
}

TEST(ThreadPoolTest, RunWithoutStartExecutesInline)
{
    ThreadPool pool("inline-pool");
    bool executed = false;
    pool.run([&executed]() { executed = true; });
    EXPECT_TRUE(executed);
}

// ---------------------------------------------------------------------------
// 新增场景（P8）
// ---------------------------------------------------------------------------

// TODO(Q21): stop() 后 run() 既不内联也不入队（src/thread/ThreadPool.cc:62-72，
// m_threads 非空且 m_isRunning=false 时静默丢弃）。本用例锁定当前行为；
// 若确认应拒绝或执行，修复后改为正向断言。
TEST(ThreadPoolTest, RunAfterStopDropsTask)
{
    ThreadPool pool("stopped-pool");
    pool.start(2);
    pool.stop();

    std::atomic<bool> executed{false};
    pool.run([&executed]() { executed.store(true); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(executed.load()); // 现状：任务被静默丢弃

    // 二次 stop 幂等（线程已 join，join 后再次 join 由 Thread::join 断言保护？
    // 实测现状：stop() 再次 join 已 join 的 pthread 行为未定义，此处不再调用）
}

/// [正] 单 worker 串行执行任务且保序（take 值返回路径）
TEST(ThreadPoolTest, SingleWorkerSerializesTasks)
{
    constexpr int kTasks = 100;
    ThreadPool pool("serial-pool");
    pool.start(1);

    std::mutex mutex;
    std::vector<int> order;
    std::latch done(kTasks);

    for (int i = 0; i < kTasks; ++i)
    {
        pool.run([&order, &mutex, &done, i]() {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(i);
            done.count_down();
        });
    }
    done.wait();
    pool.stop();

    ASSERT_EQ(order.size(), static_cast<size_t>(kTasks));
    for (int i = 0; i < kTasks; ++i)
    {
        EXPECT_EQ(order[static_cast<size_t>(i)], i) << "单 worker 应保序";
    }
}

/// [并] 8 线程 × 1000 任务并发提交守恒（take 值返回路径）
TEST(ThreadPoolTest, ConcurrentSubmitConservation)
{
    constexpr int kSubmitters = 8;
    constexpr int kTasksPerThread = 1000;
    constexpr int kTotal = kSubmitters * kTasksPerThread;

    ThreadPool pool("storm-pool");
    pool.start(4);

    std::atomic<int> executed{0};
    std::latch done(kTotal);

    std::vector<std::thread> submitters;
    for (int t = 0; t < kSubmitters; ++t)
    {
        submitters.emplace_back([&]() {
            for (int i = 0; i < kTasksPerThread; ++i)
            {
                pool.run([&executed, &done]() {
                    executed.fetch_add(1);
                    done.count_down();
                });
            }
        });
    }
    for (auto& thread : submitters)
    {
        thread.join();
    }
    done.wait();
    pool.stop();

    EXPECT_EQ(executed.load(), kTotal);
}

/// [界] start() 二次调用触发 assert——死亡测试（现状锁定）
TEST(ThreadPoolTest, StartTwiceAborts)
{
    if (CODEBUS_TEST_UNDER_SANITIZER)
    {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            ThreadPool pool("double-start");
            pool.start(2);
            pool.start(2); // assert(m_threads.empty() && !m_isRunning) 失败
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

// TODO(Q9): threadFunc 的 task() 无 try/catch（src/thread/ThreadPool.cc:40-50），
// 任务抛异常会穿透 pthread 边界触发 std::terminate（进程终止），worker 无法
// 存活。本用例以死亡测试锁定当前行为；若决定 worker 吞异常保活，修复后改为
// 正向断言（提交抛出任务后池仍可继续执行后续任务）。
TEST(ThreadPoolTest, ThrowingTaskTerminatesProcess)
{
    if (CODEBUS_TEST_UNDER_SANITIZER)
    {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            ThreadPool pool("throwing-pool");
            pool.start(1);
            std::latch done(1);
            pool.run([&done]() {
                done.count_down();
                throw std::runtime_error("task boom");
            });
            done.wait();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

} // namespace test
} // namespace bus
