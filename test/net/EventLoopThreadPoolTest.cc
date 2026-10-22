//***********************************************
// name      : EventLoopThreadPoolTest.cc
// brief     : net 模块单元测试：被测 EventLoopThread.h 内的 EventLoopThreadPool（IO 线程池）
// attention : base loop 由测试侧 LoopThread 提供（Start/销毁在宿主线程）
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"

#include <atomic>
#include <unordered_set>
#include <vector>

namespace bus
{
namespace test
{

class EventLoopThreadPoolTest : public ::testing::Test
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

    /// @brief base loop 宿主线程
    std::unique_ptr<LoopThread> m_loop_thread;
};

/// [正] 0 线程：getNextLoop 退化为 base loop
TEST_F(EventLoopThreadPoolTest, ZeroThreadsReturnsBaseLoop)
{
    EventLoopThreadPool pool(m_loop_thread->loop());
    pool.Start();

    EXPECT_TRUE(pool.isStarted());
    EXPECT_EQ(pool.getNextLoop(), m_loop_thread->loop());
    EXPECT_EQ(pool.getNextLoop(), m_loop_thread->loop());
}

/// [正] N 线程：round-robin 循环分发，各 loop 均非 base
TEST_F(EventLoopThreadPoolTest, RoundRobinDistribution)
{
    EventLoopThreadPool pool(m_loop_thread->loop());
    pool.setThreadNum(3);
    pool.Start();

    EventLoop* first = pool.getNextLoop();
    EXPECT_NE(first, m_loop_thread->loop());
    std::vector<EventLoop*> sequence{first};
    for (int i = 1; i < 6; ++i)
    {
        sequence.push_back(pool.getNextLoop());
    }
    // 周期 3：sequence[0]==sequence[3]，且前 3 个互不相同
    EXPECT_EQ(sequence[0], sequence[3]);
    EXPECT_EQ(sequence[1], sequence[4]);
    EXPECT_EQ(sequence[2], sequence[5]);
    std::unordered_set<EventLoop*> distinct(sequence.begin(), sequence.begin() + 3);
    EXPECT_EQ(distinct.size(), 3U);
}

/// [正] Start 幂等：重复调用不新增线程（init 回调总数不变）
TEST_F(EventLoopThreadPoolTest, StartIsIdempotent)
{
    std::atomic<int> init_calls{0};
    EventLoopThreadPool pool(m_loop_thread->loop());
    pool.setThreadNum(2);
    pool.Start([&](EventLoop*) { init_calls.fetch_add(1); });
    pool.Start([&](EventLoop*) { init_calls.fetch_add(1); });

    EXPECT_EQ(init_calls.load(), 2);   // 仅首轮生效
}

/// [正] init 回调在各 IO 线程内执行且各一次
TEST_F(EventLoopThreadPoolTest, InitCallbackRunsInEachLoopThread)
{
    std::atomic<int> in_thread_count{0};
    EventLoopThreadPool pool(m_loop_thread->loop());
    pool.setThreadNum(2);
    pool.Start([&](EventLoop* loop) {
        if (loop->isInLoopThread())
        {
            in_thread_count.fetch_add(1);
        }
    });

    EXPECT_EQ(in_thread_count.load(), 2);
}

/// [正] 析构停止并回收全部 IO 线程（作用域退出不挂起）
TEST_F(EventLoopThreadPoolTest, DestructorStopsAllThreads)
{
    {
        EventLoopThreadPool pool(m_loop_thread->loop());
        pool.setThreadNum(4);
        pool.Start();
    }
    SUCCEED();   // 4 线程均已 join
}

} // namespace test
} // namespace bus
