//***********************************************
// name      : BlockingQueueTest.cc
// brief     : base 模块单元测试：被测 src/base/BlockingQueue.hpp（无界/有界阻塞队列）
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "base/BlockingQueue.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

TEST(BlockingQueueTest, PutTake)
{
    BlockingQueue<int> queue;
    queue.put(42);
    EXPECT_EQ(queue.size(), 1U);
    EXPECT_FALSE(queue.isEmpty());
    EXPECT_EQ(queue.take(), 42);
    EXPECT_TRUE(queue.isEmpty());
}

TEST(BlockingQueueTest, TakeBlocksUntilPut)
{
    BlockingQueue<int> queue;
    std::atomic<bool> taken{false};
    int value = 0;

    std::thread consumer([&]() {
        value = queue.take();
        taken.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(taken.load());
    queue.put(7);
    consumer.join();

    EXPECT_TRUE(taken.load());
    EXPECT_EQ(value, 7);
}

TEST(BlockingQueueTest, DrainReturnsAllElements)
{
    BlockingQueue<int> queue;
    queue.put(1);
    queue.put(2);
    queue.put(3);

    auto drained = queue.drain();
    EXPECT_EQ(drained.size(), 3U);
    EXPECT_EQ(drained.front(), 1);
    EXPECT_EQ(drained.back(), 3);
    EXPECT_TRUE(queue.isEmpty());
}

TEST(BlockingQueueTest, OverUnblocksConsumer)
{
    BlockingQueue<int> queue;
    std::atomic<bool> returned{false};
    int value = -1;

    std::thread consumer([&]() {
        value = queue.take();
        returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    queue.over();
    consumer.join();

    EXPECT_TRUE(returned.load());
    EXPECT_EQ(value, 0); // over() 后返回默认值
}

/// [正] take() 值返回支持仅移动类型（unique_ptr），无悬垂引用
TEST(BlockingQueueTest, MoveOnlyElements)
{
    BlockingQueue<std::unique_ptr<int>> queue;
    queue.put(std::make_unique<int>(9));

    auto value = queue.take();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 9);
}

// ---------------------------------------------------------------------------
// 新增场景（P6）
// ---------------------------------------------------------------------------

/// [界] 非正容量构造抛出（0 与负值拒绝）
TEST(BoundBlockingQueueTest, ConstructorRejectsNonPositiveCapacity)
{
    EXPECT_ANY_THROW(BoundBlockingQueue<int> queue(0));
    EXPECT_ANY_THROW(BoundBlockingQueue<int> queue(-1));
    EXPECT_NO_THROW(BoundBlockingQueue<int> queue(1));
}

/// [正] 有界队列容量边界、put/take 与 drain 取空
TEST(BoundBlockingQueueTest, BoundedPutTakeAndDrain)
{
    BoundBlockingQueue<int> queue(2);
    queue.put(1);
    queue.put(2);
    EXPECT_TRUE(queue.isFull());

    EXPECT_EQ(queue.take(), 1);
    EXPECT_FALSE(queue.isFull());
    EXPECT_EQ(queue.take(), 2);
    EXPECT_TRUE(queue.isEmpty());

    queue.put(7);
    std::queue<int> drained = queue.drain();
    EXPECT_EQ(drained.size(), 1U);
    EXPECT_EQ(drained.front(), 7);
    EXPECT_TRUE(queue.isEmpty());
}

/// [正] FIFO 严格顺序：N 个元素按入队顺序出队
TEST(BlockingQueueTest, FifoOrder)
{
    BlockingQueue<int> queue;
    constexpr int kCount = 100;

    for (int i = 0; i < kCount; ++i)
    {
        queue.put(i);
    }

    for (int i = 0; i < kCount; ++i)
    {
        EXPECT_EQ(queue.take(), i) << "第 " << i << " 个元素乱序";
    }
}

/// [并] 4 生产者 × 4 消费者交换 10000 元素：总数守恒
TEST(BlockingQueueTest, ProducerConsumerConservation)
{
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 2500;
    constexpr int kTotal = kProducers * kPerProducer;

    BlockingQueue<int> queue;
    std::atomic<int> consumed{0};

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c)
    {
        consumers.emplace_back([&]() {
            while (queue.take() != -1) // 以 -1 为结束哨兵
            {
                consumed.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&]() {
            for (int i = 0; i < kPerProducer; ++i)
            {
                queue.put(1);
            }
        });
    }
    for (auto& thread : producers)
    {
        thread.join();
    }
    for (int c = 0; c < kConsumers; ++c)
    {
        queue.put(-1); // 每个消费者一个结束哨兵
    }
    for (auto& thread : consumers)
    {
        thread.join();
    }

    EXPECT_EQ(consumed.load(), kTotal);
    EXPECT_TRUE(queue.isEmpty());
}

// TODO(Q6): over() 后 put() 仍成功入队（不检查 m_isRunning，
// src/base/BlockingQueue.hpp:247-260），随后 take() 可取到该元素。
// 本用例锁定当前行为；若确认 over 后应拒绝入队，修复后改为正向断言。
TEST(BlockingQueueTest, PutAfterOverStillSucceeds)
{
    BlockingQueue<int> queue;
    queue.over();

    queue.put(99); // 现状：静默成功
    EXPECT_EQ(queue.size(), 1U);
    EXPECT_EQ(queue.take(), 99);
}

/// [界] over() 后 take() 先排空剩余元素再返回默认值
TEST(BlockingQueueTest, TakeAfterOverDrainsRemaining)
{
    BlockingQueue<int> queue;
    queue.put(1);
    queue.put(2);
    queue.over();

    EXPECT_EQ(queue.take(), 1); // 剩余元素仍可取
    EXPECT_EQ(queue.take(), 2);
    EXPECT_EQ(queue.take(), 0); // 排空后返回默认值
}

} // namespace test
} // namespace bus
