//***********************************************
// name      : EventLoopThreadTest.cc
// brief     : net 模块单元测试：被测 src/net/EventLoopThread.h/.cc（IO 线程生命周期）
// attention : EventLoop 的创建、Loop、销毁均在 IO 线程内完成（被测对象保证）
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"

#include <sys/resource.h>
#include <unistd.h>

#include <atomic>

namespace bus
{
namespace test
{

/// [正] StartLoop 同步返回就绪的 loop，RunInLoop 在 IO 线程执行
TEST(EventLoopThreadTest, StartLoopReturnsReadyLoop)
{
    EventLoopThread thread;
    EventLoop* loop = thread.StartLoop();
    ASSERT_NE(loop, nullptr);

    std::atomic<bool> ran{false};
    std::atomic<bool> in_loop_thread{false};
    loop->RunInLoop([&]() {
        in_loop_thread.store(loop->isInLoopThread());
        ran.store(true);
    });
    EXPECT_TRUE(WaitFor([&]() { return ran.load(); }));
    EXPECT_TRUE(in_loop_thread.load());
    // 析构自动 Stop + join（本用例即验证该路径）
}

/// [正] 初始化回调在 loop 线程内、Loop() 之前执行且仅一次
TEST(EventLoopThreadTest, InitCallbackRunsInLoopThreadOnce)
{
    std::atomic<int> init_calls{0};
    std::atomic<bool> init_in_thread{false};
    {
        EventLoopThread thread([&](EventLoop* loop) {
            init_in_thread.store(loop->isInLoopThread());
            init_calls.fetch_add(1);
        });
        EventLoop* loop = thread.StartLoop();
        ASSERT_NE(loop, nullptr);

        std::atomic<bool> ran{false};
        loop->RunInLoop([&]() { ran.store(true); });
        EXPECT_TRUE(WaitFor([&]() { return ran.load(); }));
        EXPECT_EQ(init_calls.load(), 1);
        EXPECT_TRUE(init_in_thread.load());
    }
    EXPECT_EQ(init_calls.load(), 1);   // 析构后回调未被重复触发
}

/// [正] 宿主线程主动 Stop：Loop 退出、析构等待收尾不挂起
TEST(EventLoopThreadTest, StopFromOwnerThreadThenDestruct)
{
    {
        EventLoopThread thread;
        EventLoop* loop = thread.StartLoop();

        std::atomic<bool> ran{false};
        loop->RunInLoop([&]() { ran.store(true); });
        EXPECT_TRUE(WaitFor([&]() { return ran.load(); }));

        loop->Stop();   // 宿主线程主动停；析构等待 Loop() 返回并 join
    }
    SUCCEED();          // 作用域结束未挂起即通过
}

/// [界] 未 StartLoop 即析构：安全空操作
TEST(EventLoopThreadTest, DestructorWithoutStartIsSafe)
{
    {
        EventLoopThread thread;
    }
    SUCCEED();
}

/// [界] I4：pthread_create 失败（RLIMIT_NPROC 收缩触发 EAGAIN）——
///      StartLoop 异常上抛，析构不挂死（未回滚 m_started 时 wait 永等）
TEST(EventLoopThreadTest, StartLoopFailureDoesNotHangDestructor)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root 绕过 RLIMIT_NPROC，无法触发";
    }
    rlimit original{};
    ASSERT_EQ(::getrlimit(RLIMIT_NPROC, &original), 0);
    struct RlimitGuard
    {
        rlimit limit;
        ~RlimitGuard() { ::setrlimit(RLIMIT_NPROC, &limit); }
    } guard{original};

    rlimit shrunk = original;
    shrunk.rlim_cur = 1;   // 低于当前用户线程总数 → pthread_create 得 EAGAIN
    if (shrunk.rlim_cur >= original.rlim_cur)
    {
        GTEST_SKIP() << "环境软限异常，无法收缩";
    }
    ASSERT_EQ(::setrlimit(RLIMIT_NPROC, &shrunk), 0);

    {
        EventLoopThread thread;
        EXPECT_ANY_THROW(thread.StartLoop());   // Thread::run 抛 logic_error
    }   // 未回滚 m_started 时：析构在 m_stopped_future.wait() 永等（超时红面）
    SUCCEED();
}

} // namespace test
} // namespace bus
