//***********************************************
// name      : SingletonTest.cc
// brief     : 根级基础设施模块单元测试：被测 src/Singleton.hpp（Meyers 单例模板）
// attention : Singleton<T> 构造/析构均 delete，仅经 getInstance 访问
//***********************************************
#include <gtest/gtest.h>
#include "Singleton.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

namespace
{

/// @brief 带构造计数的单例类型（统计实际构造次数）
struct Counted
{
    Counted() { constructed.fetch_add(1); }

    static std::atomic<int> constructed;
};

std::atomic<int> Counted::constructed{0};

struct OtherType
{
    int value{42};
};

} // namespace

// 编译期契约：构造/析构 deleted，不可实例化
static_assert(!std::is_constructible_v<Singleton<OtherType>>,
              "Singleton 构造函数应被删除");
static_assert(!std::is_default_constructible_v<Singleton<OtherType>>,
              "Singleton 不应有默认构造");

/// [正] 同类型多次 getInstance 返回同一地址
TEST(SingletonTest, SameInstanceEveryCall)
{
    EXPECT_EQ(Singleton<OtherType>::getInstance(), Singleton<OtherType>::getInstance());
    EXPECT_EQ(Singleton<OtherType>::getInstance()->value, 42);
}

/// [界] 不同类型的实例互不相同
TEST(SingletonTest, DistinctAcrossTypes)
{
    struct Another
    {
        int value{1};
    };

    EXPECT_NE(static_cast<void*>(Singleton<OtherType>::getInstance()),
              static_cast<void*>(Singleton<Another>::getInstance()));
}

/// [并] 8 线程并发首次获取：仅构造一次且全部获得同一实例
TEST(SingletonTest, ConcurrentFirstInitConstructsOnce)
{
    Counted::constructed.store(0);

    constexpr int kThreads = 8;
    std::vector<Counted*> observed(kThreads, nullptr);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&observed, t]() {
            observed[static_cast<size_t>(t)] = Singleton<Counted>::getInstance();
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    // Meyers 单例的线程安全由 C++11 保证：首个调用构造，其余等待
    for (Counted* pointer : observed)
    {
        EXPECT_EQ(pointer, observed[0]);
    }
    EXPECT_EQ(Counted::constructed.load(), 1);
}

} // namespace test
} // namespace bus
