//***********************************************
// name      : TimerTest.cc
// brief     : Timer 模块单元测试：被测 src/Timer.h/.cc（Timer + TimerManager 两套件）
// attention : TimerManager 必须在 loop 线程构造与析构（经 ManagerGuard
//             RAII 保证）
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "Timer.h"
#include "net/EventLoop.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

using namespace std::chrono_literals;

class TimerTest : public ::testing::Test
{
 protected:
    void SetUp() override
    {
        m_loop_thread = std::make_unique<LoopThread>();
        m_loop = m_loop_thread->loop();
    }

    void TearDown() override
    {
        m_loop_thread.reset();
    }

    /// @brief 运行事件循环的线程
    std::unique_ptr<LoopThread> m_loop_thread;
    /// @brief 事件循环
    EventLoop* m_loop{nullptr};
};

/// @brief loop 线程构造/析构的 TimerManager RAII 守卫
/// @attention Channel 的 Enable/Disable 经 EventLoop::UpdateChannel 有线程
///            断言，构造与析构都必须发生在 loop 线程（经 RunInLoopSync 保证）
class ManagerGuard
{
 public:
    explicit ManagerGuard(EventLoop* loop) : m_loop(loop)
    {
        RunInLoopSync(m_loop, [this]() {
            m_manager = std::make_unique<TimerManager>(std::ref(*m_loop));
        });
    }

    ~ManagerGuard()
    {
        RunInLoopSync(m_loop, [this]() { m_manager.reset(); });
    }

    ManagerGuard(const ManagerGuard&) = delete;
    ManagerGuard& operator=(const ManagerGuard&) = delete;

    ATTR_PURE_INLINE
    TimerManager* get() const noexcept
    {
        return m_manager.get();
    }

 private:
    /// @brief 所属事件循环
    EventLoop* m_loop;
    /// @brief 被守护的 TimerManager
    std::unique_ptr<TimerManager> m_manager;
};

TEST_F(TimerTest, OneShotTimerFiresOnce)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> fired{0};

    manager->addTimer([&]() { fired.fetch_add(1); },
                      std::chrono::steady_clock::now() + 100ms, 0ms);

    ASSERT_TRUE(WaitFor([&]() { return fired.load() == 1; },
                        std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(fired.load(), 1);
}

TEST_F(TimerTest, PeriodicTimerFiresMultipleTimes)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> fired{0};

    auto timer = manager->addTimer([&]() { fired.fetch_add(1); },
                                   std::chrono::steady_clock::now() + 100ms, 100ms);

    ASSERT_TRUE(WaitFor([&]() { return fired.load() >= 3; }, std::chrono::milliseconds(3000)));
    timer->cancel();

    const int count_after_cancel = fired.load();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(fired.load(), count_after_cancel);
    EXPECT_TRUE(timer->isRepeat());
    EXPECT_TRUE(timer->isCanceled());
}

TEST_F(TimerTest, CancelPreventsFire)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> fired{0};

    auto timer = manager->addTimer([&]() { fired.fetch_add(1); },
                                   std::chrono::steady_clock::now() + 200ms, 0ms);
    timer->cancel();

    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(fired.load(), 0);
    EXPECT_TRUE(timer->isCanceled());
}

TEST_F(TimerTest, Properties)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    auto timer = manager->addTimer([]() {}, std::chrono::steady_clock::now() + 200ms, 50ms);

    EXPECT_TRUE(timer->isRepeat());
    EXPECT_EQ(timer->getRepeatTime(), 50ms);
    EXPECT_FALSE(timer->isCanceled());
    EXPECT_GT(timer->Expiration().count(), 0);

    timer->cancel();
    EXPECT_TRUE(timer->isCanceled());
}

TEST_F(TimerTest, CrossThreadAddTimer)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> fired{0};

    // 主线程调用 addTimer，实际插入在 loop 线程
    manager->addTimer([&]() { fired.fetch_add(1); },
                      std::chrono::steady_clock::now() + 100ms, 0ms);

    ASSERT_TRUE(WaitFor([&]() { return fired.load() == 1; }, std::chrono::milliseconds(3000)));
}

TEST_F(TimerTest, ConcurrentAddAndCancel)
{
    constexpr int kThreads = 8;
    constexpr int kTimersPerThread = 100;
    constexpr int kExpectFired = kThreads * kTimersPerThread / 2;

    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> fired{0};
    std::atomic<int> sequence{0};

    std::vector<std::thread> producers;
    producers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        producers.emplace_back([&, t]() {
            for (int i = 0; i < kTimersPerThread; ++i)
            {
                const auto deadline = std::chrono::steady_clock::now() + 300ms +
                                      std::chrono::nanoseconds(sequence.fetch_add(1));
                auto timer = manager->addTimer([&]() { fired.fetch_add(1); }, deadline, 0ms);

                if ((t + i) % 2 != 0)
                {
                    timer->cancel();
                }
            }
        });
    }
    for (auto& thread : producers)
    {
        thread.join();
    }

    ASSERT_TRUE(WaitFor([&]() { return fired.load() >= kExpectFired; },
                        std::chrono::milliseconds(5000)))
        << "fired=" << fired.load();

    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(fired.load(), kExpectFired);
}

/// [命] 析构关闭 timerfd 并摘除 Channel：100 次创建/销毁 loop 线程 fd 零净增
TEST_F(TimerTest, ManagerDestructorClosesTimerFd)
{
    const int before = RunInLoopSync(m_loop, []() { return FdCount(); });

    RunInLoopSync(m_loop, [this]() {
        for (int i = 0; i < 100; ++i)
        {
            TimerManager manager(std::ref(*m_loop));
            (void)manager;
        }
    });

    const int after = RunInLoopSync(m_loop, []() { return FdCount(); });
    EXPECT_LE(after, before + 1) << "before=" << before << " after=" << after;
}

/// [正] refresh 重瞄周期定时器：重瞄后首触时刻 = refresh + repeatTime
/// @attention 延迟 80ms 再 refresh，使重瞄点（T+280）与原到期点（T+200）
///            拉开间隙；T+230 检查点在原到期点之后、重瞄点之前——refresh
///            失效则原 timer 提前触发，fired==0 断言即红（证伪点）
TEST_F(TimerTest, RefreshReArmsTimer)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> fired{0};

    const auto start = std::chrono::steady_clock::now();
    auto timer = manager->addTimer([&]() { fired.fetch_add(1); },
                                   std::chrono::steady_clock::now() + 200ms, 200ms);

    std::this_thread::sleep_for(80ms);   // 走过 80ms 后重瞄（首触推至 ~T+280）
    timer->refresh();

    // 证伪窗口：原到期点 T+200 已过而重瞄点未到，不得有触发
    std::this_thread::sleep_until(start + 230ms);
    EXPECT_EQ(fired.load(), 0) << "refresh 后首触发早于重瞄点（refresh 未生效）";

    ASSERT_TRUE(WaitFor([&]() { return fired.load() >= 2; }, std::chrono::milliseconds(3000)));
}

// ---------------------------------------------------------------------------
// 新增场景（P5）
// ---------------------------------------------------------------------------

/// [正] 已过期的到期时间（now - 1s）添加后立即触发
TEST_F(TimerTest, ExpiredDeadlineFiresImmediately)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> fired{0};

    manager->addTimer([&]() { fired.fetch_add(1); },
                      std::chrono::steady_clock::now() - 1s, 0ms);

    ASSERT_TRUE(WaitFor([&]() { return fired.load() == 1; },
                        std::chrono::milliseconds(2000)));
}

/// [界] 周期间隔容差：100ms 周期连续触发的间隔在 [60, 250]ms 内（上界对
/// 负载抖动/TSAN 慢环境留裕量，下界防定时器合并）
TEST_F(TimerTest, PeriodicIntervalWithinTolerance)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();

    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> stamps;

    auto timer = manager->addTimer(
        [&]() {
            std::lock_guard<std::mutex> lock(mutex);
            stamps.push_back(std::chrono::steady_clock::now());
        },
        std::chrono::steady_clock::now() + 50ms, 100ms);

    ASSERT_TRUE(WaitFor(
        [&]() {
            std::lock_guard<std::mutex> lock(mutex);
            return stamps.size() >= 5;
        },
        std::chrono::milliseconds(3000)));

    timer->cancel();

    std::vector<std::chrono::steady_clock::time_point> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = stamps;
    }

    ASSERT_GE(snapshot.size(), 5U);
    for (size_t i = 1; i < snapshot.size(); ++i)
    {
        const auto interval =
            std::chrono::duration_cast<std::chrono::milliseconds>(snapshot[i] - snapshot[i - 1]);
        EXPECT_GE(interval.count(), 60) << "interval[" << i << "]=" << interval.count();
        EXPECT_LE(interval.count(), 250) << "interval[" << i << "]=" << interval.count();
    }
}

/// [正] 回调执行期间添加新定时器（TimerManager 处理过期任务时的插入路径）
TEST_F(TimerTest, AddTimerFromCallbackFires)
{
    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> first{0};
    std::atomic<int> second{0};

    manager->addTimer(
        [&]() {
            first.fetch_add(1);
            // 在定时器回调内新增定时器（触发 Manager 的处理期插入分支）
            manager->addTimer([&]() { second.fetch_add(1); },
                              std::chrono::steady_clock::now() + 50ms, 0ms);
        },
        std::chrono::steady_clock::now() + 50ms, 0ms);

    ASSERT_TRUE(WaitFor([&]() { return first.load() == 1; }, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(WaitFor([&]() { return second.load() == 1; }, std::chrono::milliseconds(3000)));
}

/// [并] 并发加深：16 线程 × 100 定时器，半数取消
TEST_F(TimerTest, ConcurrentAddAndCancel16Threads)
{
    constexpr int kThreads = 16;
    constexpr int kTimersPerThread = 100;
    constexpr int kExpectFired = kThreads * kTimersPerThread / 2;

    ManagerGuard manager_guard(m_loop);
    TimerManager* manager = manager_guard.get();
    std::atomic<int> fired{0};
    std::atomic<int> sequence{0};

    std::vector<std::thread> producers;
    producers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        producers.emplace_back([&, t]() {
            for (int i = 0; i < kTimersPerThread; ++i)
            {
                const auto deadline = std::chrono::steady_clock::now() + 400ms +
                                      std::chrono::nanoseconds(sequence.fetch_add(1));
                auto timer = manager->addTimer([&]() { fired.fetch_add(1); }, deadline, 0ms);

                if ((t + i) % 2 != 0)
                {
                    timer->cancel();
                }
            }
        });
    }
    for (auto& thread : producers)
    {
        thread.join();
    }

    ASSERT_TRUE(WaitFor([&]() { return fired.load() >= kExpectFired; },
                        std::chrono::milliseconds(8000)))
        << "fired=" << fired.load();

    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(fired.load(), kExpectFired);
}

} // namespace test
} // namespace bus
