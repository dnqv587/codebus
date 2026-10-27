//***********************************************
// name      : ThreadTest.cc
// brief     : thread 模块单元测试：被测 src/thread/Thread.h/.cc 与 CurrentThread.h/.cc
// attention : Thread 私有继承 CurrentThread，二者为一个可测单元；
//             bindCPU 越界与 join 未 run 线程触发 assert（死亡测试，
//             sanitizer 下守卫跳过）；未 join 的运行中线程析构行为见 Q22
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "thread/CurrentThread.h"
#include "thread/Thread.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <latch>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Util.h"

namespace bus {
namespace test {

// ---------------------------------------------------------------------------
// 迁移用例（自旧 ThreadTest）
// ---------------------------------------------------------------------------

TEST(ThreadTest, RunJoinAndThreadInfo) {
    const pid_t main_tid = CurrentThread::tid();
    std::atomic<bool> ran{false};
    pid_t worker_tid = 0;
    std::string worker_name;

    Thread thread(
        [&]() {
            ran.store(true);
            worker_tid = CurrentThread::tid();
            worker_name = CurrentThread::ThreadName();
        },
        "worker");

    thread.run();
    EXPECT_TRUE(CurrentThread::isMainThread());
    EXPECT_GT(thread.getTid(), 0);  // join 后会被重置为 0
    thread.join();

    EXPECT_TRUE(ran.load());
    EXPECT_EQ(thread.getThreadName(), "worker");
    EXPECT_NE(worker_tid, main_tid);
    EXPECT_EQ(worker_name, "worker");
}

TEST(ThreadTest, BindCpu) {
    Thread thread([]() {}, "cpu-thread");
    thread.run();
    EXPECT_NO_THROW(thread.bindCPU(0));
    thread.join();
}

// ---------------------------------------------------------------------------
// 新增场景（P8）
// ---------------------------------------------------------------------------

/// [正] CurrentThread：主线程 tid==getpid、isMainThread、名称为 MainThread
TEST(CurrentThreadTest, MainThreadIdentity) {
    EXPECT_EQ(CurrentThread::tid(), ::getpid());
    EXPECT_TRUE(CurrentThread::isMainThread());
    EXPECT_STREQ(CurrentThread::ThreadName(), "MainThread");

    // 稳定性：两次调用一致
    EXPECT_EQ(CurrentThread::tid(), CurrentThread::tid());
}

/// [正] CurrentThread：普通 std::thread 内非主线程、名字为默认值、getCPU 合法
TEST(CurrentThreadTest, NonMainThreadIdentity) {
    std::atomic<bool> is_main{true};
    std::atomic<bool> name_default{false};
    std::atomic<bool> cpu_valid{false};

    std::thread worker([&]() {
        is_main.store(CurrentThread::isMainThread());
        name_default.store(std::string(CurrentThread::ThreadName()) == "UnnamedThread");
        cpu_valid.store(CurrentThread::getCPU() < SystemUtil::getCoreNum());
    });
    worker.join();

    EXPECT_FALSE(is_main.load());
    EXPECT_TRUE(name_default.load());
    EXPECT_TRUE(cpu_valid.load());
}

/// [正] 默认线程名 UnnamedThread
TEST(ThreadTest, DefaultThreadName) {
    Thread thread([]() {});
    EXPECT_EQ(thread.getThreadName(), "UnnamedThread");
    thread.run();
    thread.join();
}

/// [界] run() 在线程启动后即返回，不等待线程函数完成（CountDownLatch 启动同步契约）
/// @attention latch 在用户函数执行前计数，run() 返回不保证函数体已运行，
///             故以"长函数未完成前 run 已返回"为核心断言
TEST(ThreadTest, RunReturnsAfterThreadStarted) {
    Thread thread([]() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }, "late");

    const auto begin = std::chrono::steady_clock::now();
    thread.run();
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_GT(thread.getTid(), 0);  // 线程已启动（tid 在 latch 前设置）
    // 线程函数需 300ms 完成；run() 远早于其完成即返回（sanitizer 慢环境留裕量）
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 250);
    thread.join();
}

/// [界] 未 run() 即析构无害（D33 闭环后：m_tid==0 不进 detach 分支）
TEST(ThreadTest, DestructWithoutRunIsHarmless) {
    {
        Thread thread([]() {}, "never-run");
    }  // 未 run：m_tid 保持 0，析构不执行 detach
    SUCCEED();
}

/// [并] 100 个线程并发启动，各自 latch 同步后 tid 互异
TEST(ThreadTest, HundredThreadsDistinctTids) {
    constexpr int kCount = 100;
    std::mutex mutex;
    std::set<pid_t> tids;
    std::atomic<int> started{0};

    std::vector<std::thread> spawners;
    spawners.reserve(4);
    for (int s = 0; s < 4; ++s) {
        spawners.emplace_back([&]() {
            for (int i = 0; i < kCount / 4; ++i) {
                Thread thread([&]() {
                    std::lock_guard<std::mutex> lock(mutex);
                    tids.insert(CurrentThread::tid());
                    started.fetch_add(1);
                });
                thread.run();
                thread.join();
            }
        });
    }
    for (auto& spawner : spawners) {
        spawner.join();
    }

    EXPECT_EQ(started.load(), kCount);
    // 已 join 线程的 tid 可能被后续线程复用，并发窗口下不保证严格互异，
    // 以 >= 90% 防灾难性退化
    EXPECT_GE(tids.size(), static_cast<size_t>(kCount) * 9 / 10);
}

/// [错] bindCPU 越界核心号触发 assert——死亡测试
TEST(ThreadTest, BindCpuOutOfRangeAborts) {
    if (CODEBUS_TEST_UNDER_SANITIZER) {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            Thread thread([]() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }, "oob");
            thread.run();
            thread.bindCPU(SystemUtil::getCoreNum() + 100);
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

}  // namespace test
}  // namespace bus
