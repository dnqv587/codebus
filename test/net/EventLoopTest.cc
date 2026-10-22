//***********************************************
// name      : EventLoopTest.cc
// brief     : net 模块单元测试：被测 src/net/EventLoop.h/.cc（事件循环、跨线程任务、唤醒、生命周期）
// attention : EventLoop 必须在创建它的线程运行 Loop 与析构；含 EXPECT_EXIT
//             死亡测试（需 Debug 构建，sanitizer 构建下由守卫宏跳过）；
//             Q3/Q4 现状锁定用例记录异常逃逸与 Stop 后任务丢弃行为；
//             D14（epoll fd 泄漏）用例在 EventDispatcherTest
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/EventLoop.h"

#include <chrono>
#include <csignal>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

class EventLoopTest : public ::testing::Test
{
 protected:
    void SetUp() override
    {
        m_loop_thread = std::make_unique<LoopThread>();
    }

    void TearDown() override
    {
        m_loop_thread.reset();
    }

    /// @brief 运行事件循环的线程
    std::unique_ptr<LoopThread> m_loop_thread;
};

TEST_F(EventLoopTest, IsInLoopThread)
{
    EventLoop* loop = m_loop_thread->loop();
    EXPECT_FALSE(loop->isInLoopThread());

    const bool in_loop = RunInLoopSync(loop, [loop]() { return loop->isInLoopThread(); });
    EXPECT_TRUE(in_loop);
}

TEST_F(EventLoopTest, RunInLoopInlineInLoopThread)
{
    EventLoop* loop = m_loop_thread->loop();
    bool executed = false;

    RunInLoopSync(loop, [&]() {
        loop->RunInLoop([&]() { executed = true; });
        EXPECT_TRUE(executed); // 同线程调用应立即执行
    });
}

TEST_F(EventLoopTest, RunInLoopPreservesOrder)
{
    constexpr int kTaskCount = 1000;
    EventLoop* loop = m_loop_thread->loop();

    std::vector<int> order;
    order.reserve(kTaskCount);
    std::latch done(kTaskCount);

    for (int i = 0; i < kTaskCount; ++i)
    {
        loop->RunInLoop([&order, &done, i]() {
            order.push_back(i);
            done.count_down();
        });
    }
    done.wait();

    ASSERT_EQ(order.size(), static_cast<size_t>(kTaskCount));
    for (int i = 0; i < kTaskCount; ++i)
    {
        EXPECT_EQ(order[static_cast<size_t>(i)], i);
    }
}

TEST_F(EventLoopTest, RunInLoopTaskStorm)
{
    constexpr int kThreadCount = 8;
    constexpr int kTasksPerThread = 5000;
    constexpr int kTotal = kThreadCount * kTasksPerThread;

    EventLoop* loop = m_loop_thread->loop();
    std::atomic<int> executed{0};
    std::atomic<bool> all_in_loop{true};
    std::latch done(kTotal);

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t)
    {
        producers.emplace_back([loop, &executed, &all_in_loop, &done]() {
            for (int i = 0; i < kTasksPerThread; ++i)
            {
                loop->RunInLoop([loop, &executed, &all_in_loop, &done]() {
                    if (!loop->isInLoopThread())
                    {
                        all_in_loop.store(false);
                    }
                    executed.fetch_add(1);
                    done.count_down();
                });
            }
        });
    }
    for (auto& thread : producers)
    {
        thread.join();
    }

    done.wait();
    EXPECT_EQ(executed.load(), kTotal);
    EXPECT_TRUE(all_in_loop.load());
}

TEST_F(EventLoopTest, WakeupStorm)
{
    constexpr int kThreadCount = 8;
    constexpr int kWakeupsPerThread = 1000;
    constexpr int kTasksPerThread = 100;

    EventLoop* loop = m_loop_thread->loop();
    std::atomic<int> executed{0};
    std::latch done(kThreadCount * kTasksPerThread);

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t)
    {
        producers.emplace_back([loop, &executed, &done]() {
            for (int i = 0; i < kWakeupsPerThread; ++i)
            {
                loop->Wakeup();
            }
            for (int i = 0; i < kTasksPerThread; ++i)
            {
                loop->RunInLoop([&executed, &done]() {
                    executed.fetch_add(1);
                    done.count_down();
                });
            }
        });
    }
    for (auto& thread : producers)
    {
        thread.join();
    }

    done.wait();
    EXPECT_EQ(executed.load(), kThreadCount * kTasksPerThread);
}

TEST_F(EventLoopTest, LoopCanBeCreatedAndDestroyed)
{
    // 事件循环的正常创建/销毁（析构关 epoll fd 见 EventDispatcherTest.DestructorClosesEpollFd，D14 已闭环）
    for (int i = 0; i < 10; ++i)
    {
        EventLoop loop;
        EXPECT_TRUE(loop.isInLoopThread());
    }
}

// ---------------------------------------------------------------------------
// 新增场景（P3）
// ---------------------------------------------------------------------------

/// [正] 跨线程 RunInLoop 排队执行：投递后不立即执行，由下一轮 doPendingFunctor 执行
/// @attention QueueInLoop 为私有接口，本用例经公有 API 从外部观测排队语义
TEST_F(EventLoopTest, CrossThreadRunInLoopIsDeferred)
{
    EventLoop* loop = m_loop_thread->loop();
    std::atomic<bool> executed{false};

    loop->RunInLoop([&]() { executed.store(true); });

    // 排队语义：投递瞬间不应执行（loop 可能正阻塞在 epoll_wait）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // 注意：无法严格断言"未执行"（唤醒可能极快），故只断言最终执行；
    // 延迟性由 Wakeup 机制保证，此处验证可达性
    EXPECT_TRUE(WaitFor([&]() { return executed.load(); }));
}

/// [界] 嵌套 RunInLoop：loop 线程任务内再 RunInLoop 同步内联执行（不重入死锁）
TEST_F(EventLoopTest, NestedRunInLoopExecutesInline)
{
    EventLoop* loop = m_loop_thread->loop();
    std::vector<int> order;

    RunInLoopSync(loop, [&]() {
        order.push_back(1);
        loop->RunInLoop([&]() { order.push_back(2); }); // 同线程 → 立即执行
        order.push_back(3);
    });

    ASSERT_EQ(order.size(), 3U);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2); // 内联于外层任务
    EXPECT_EQ(order[2], 3);
}

// TODO(Q4): Stop() 之后投递的任务被静默丢弃（不执行也无告警，
// src/net/EventLoop.cc:114-141）。本用例锁定当前行为；若确认为缺陷并
// 登记 D 编号后，改为正向断言（执行或显式拒绝）。
TEST_F(EventLoopTest, TasksAfterStopAreDropped)
{
    std::promise<EventLoop*> ready;
    std::promise<void> loop_stopped;
    std::promise<void> may_delete;
    auto stopped_future = loop_stopped.get_future();
    auto delete_future = may_delete.get_future().share();

    // EventLoop 必须在创建线程析构（~EventLoop 摘 Channel 有线程断言）
    std::thread loop_thread([&]() {
        auto* loop = new EventLoop;
        ready.set_value(loop);
        loop->Loop();
        loop_stopped.set_value();
        delete_future.wait();
        delete loop;
    });

    EventLoop* loop = ready.get_future().get();
    loop->Stop();
    stopped_future.wait(); // Loop() 已返回，对象仍存活

    std::atomic<bool> executed{false};
    loop->RunInLoop([&]() { executed.store(true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(executed.load()); // 现状：任务永不执行

    may_delete.set_value();
    loop_thread.join();
}

/// [界] 同一线程构造第二个 EventLoop 触发断言（one loop per thread）——死亡测试
TEST_F(EventLoopTest, SecondLoopInSameThreadAborts)
{
    if (CODEBUS_TEST_UNDER_SANITIZER)
    {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            EventLoop first;
            EventLoop second; // t_ThisThreadLoop 断言失败
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

// TODO(Q3): pending functor 抛出的异常会逃逸 Loop()（doPendingFunctor 无
// try/catch，src/net/EventLoop.cc:150-153），未捕获异常终止 IO 线程与进程。
// 本用例锁定当前行为；若决定吞异常保活（记日志），本用例改为正向断言。
TEST_F(EventLoopTest, FunctorExceptionEscapesLoopAndTerminates)
{
    if (CODEBUS_TEST_UNDER_TSAN)
    {
        // TSAN 不支持多线程 fork 后再建线程（子进程需新建 io_thread）
        GTEST_SKIP() << "death test skipped under TSan (fork+thread unsupported)";
    }
    EXPECT_EXIT(
        {
            std::promise<EventLoop*> ready;
            std::thread io_thread([&]() {
                auto* loop = new EventLoop;
                ready.set_value(loop);
                loop->Loop();
                delete loop;
            });
            EventLoop* loop = ready.get_future().get();
            loop->RunInLoop([]() { throw std::runtime_error("boom"); });
            io_thread.join(); // 异常逃逸 Loop() → 未捕获 → std::terminate
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

/// [并] 16 线程 RunInLoop 任务与 Wakeup 混合风暴（8×500 任务 + 8×500 唤醒）
TEST_F(EventLoopTest, MixedTaskAndWakeupStorm16Threads)
{
    constexpr int kThreadCount = 16;
    constexpr int kOpsPerThread = 500;
    constexpr int kTaskThreads = 8;
    constexpr int kTotalTasks = kTaskThreads * kOpsPerThread;

    EventLoop* loop = m_loop_thread->loop();
    std::atomic<int> executed{0};
    std::latch done(kTotalTasks);

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t)
    {
        const bool is_task_thread = t < kTaskThreads;
        producers.emplace_back([loop, &executed, &done, is_task_thread]() {
            for (int i = 0; i < kOpsPerThread; ++i)
            {
                if (is_task_thread)
                {
                    loop->RunInLoop([&executed, &done]() {
                        executed.fetch_add(1);
                        done.count_down();
                    });
                }
                else
                {
                    loop->Wakeup();
                }
            }
        });
    }
    for (auto& thread : producers)
    {
        thread.join();
    }

    done.wait();
    EXPECT_EQ(executed.load(), kTotalTasks);
}

/// [并] Wakeup/任务投递与 Stop 并发竞争：循环干净退出，无崩溃无挂起
TEST_F(EventLoopTest, WakeupStopRace)
{
    constexpr int kThreadCount = 8;
    constexpr int kOpsPerThread = 300;

    EventLoop* loop = m_loop_thread->loop();
    std::atomic<int> executed{0};

    std::vector<std::thread> spammers;
    spammers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t)
    {
        spammers.emplace_back([loop, &executed, t]() {
            for (int i = 0; i < kOpsPerThread; ++i)
            {
                if (t % 2 == 0)
                {
                    loop->Wakeup();
                }
                else
                {
                    loop->RunInLoop([&executed]() { executed.fetch_add(1); });
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loop->Stop(); // 与并发投递竞争

    for (auto& thread : spammers)
    {
        thread.join();
    }
    m_loop_thread.reset(); // join loop 线程，验证干净退出

    // Stop 前已入队任务可能部分执行；只断言上界与无崩溃
    EXPECT_LE(executed.load(), 4 * kOpsPerThread);
}

} // namespace test
} // namespace bus
