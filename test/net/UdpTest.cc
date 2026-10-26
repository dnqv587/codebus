//***********************************************
// name      : UdpTest.cc
// brief     : net 模块单元测试：被测 src/net/UdpServer.h/.cc（同步模型：
//             接收主线程 + 任务线程池分发）
// attention : MessageCallback 在工作者线程执行；跨数据报仅 worker_num=1
//             时保序（FIFO 队列 + 单工作者）
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "Buffer.hpp"
#include "net/Address.h"
#include "net/Socket.h"
#include "net/UdpServer.h"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

/// @brief 以 raw Socket 为对端客户端发送一条数据报
void SendDatagram(Socket& client, const Address& to, std::string_view payload)
{
    ASSERT_TRUE(client.sendTo(payload.data(), payload.size(), to));
}

/// [正] 同步回显：raw 客户端 → 工作者线程回调（数据+对端）→ sendTo 回源
TEST(UdpTest, SyncEcho)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);

    std::atomic<bool> got{false};
    std::string received;
    server.setMessageCallback([&](std::shared_ptr<Buffer> data, const Address& peer) {
        received.assign(data->PeekAll().toString());
        server.sendTo(std::string_view("ack"), peer);   // 工作者线程内回源
        got.store(true);
    });
    server.Start();

    Socket client(Socket::IPV4, Socket::NIO_UDP);
    ASSERT_TRUE(client.isAvailable());
    SendDatagram(client, server.localAddr(), "ping");

    ASSERT_TRUE(WaitFor([&]() { return got.load(); }));
    EXPECT_EQ(received, "ping");

    char buf[16]{};
    const auto [n, from] = client.recvFrom(buf, sizeof(buf));
    ASSERT_EQ(n, 3);
    EXPECT_EQ(std::string_view(buf, 3), "ack");
    EXPECT_EQ(from.port(), port);

    server.Stop();
}

/// [正] 多工作者：K 条数据报全部处理（带序号；总数断言——并行模式不保证顺序）
TEST(UdpTest, ParallelWorkersProcessAll)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    constexpr int kCount = 50;
    UdpServer server("127.0.0.1:" + std::to_string(port), 2);

    std::atomic<int> processed{0};
    server.setMessageCallback([&](const std::shared_ptr<Buffer>&, const Address&) {
        processed.fetch_add(1);
    });
    server.Start();

    Socket client(Socket::IPV4, Socket::NIO_UDP);
    ASSERT_TRUE(client.isAvailable());
    for (int i = 0; i < kCount; ++i)
    {
        SendDatagram(client, server.localAddr(), "seq:" + std::to_string(i));
    }
    EXPECT_TRUE(WaitFor([&]() { return processed.load() == kCount; },
                        std::chrono::milliseconds(5000)));

    server.Stop();
    EXPECT_EQ(processed.load(), kCount);
}

/// [正] 单工作者保序：worker_num=1 → FIFO 队列 + 单工作者，序号严格有序
TEST(UdpTest, SingleWorkerPreservesOrder)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    constexpr int kCount = 30;
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);

    std::mutex mutex;
    std::vector<int> order;
    server.setMessageCallback([&](std::shared_ptr<Buffer> data, const Address&) {
        // "seq:N" → N（发送侧保证唯一序号）。Buffer 无 NUL 终止——
        // 消费必须以 ReadableSize 界定（拷贝到栈缓冲补 NUL 再解析）
        char text[16]{};
        const size_t len = std::min<size_t>(data->ReadableSize(), sizeof(text) - 1);
        std::memcpy(text, data->PeekAll().data(), len);
        int seq = -1;
        sscanf(text, "seq:%d", &seq);
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(seq);
    });
    server.Start();

    Socket client(Socket::IPV4, Socket::NIO_UDP);
    ASSERT_TRUE(client.isAvailable());
    for (int i = 0; i < kCount; ++i)
    {
        SendDatagram(client, server.localAddr(), "seq:" + std::to_string(i));
    }
    ASSERT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return order.size() == kCount;
    }, std::chrono::milliseconds(5000)));

    std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < kCount; ++i)
    {
        EXPECT_EQ(order[i], i) << "第 " << i << " 条乱序";
    }

    server.Stop();
}

/// [正] D41 闭环：Stop 经 ThreadPool::stopAndDrain 排空已提交任务（端到端
///      弱形态：等全部处理完再 Stop 防回归——强断言在 ThreadPoolTest.
///      StopAndDrainExecutesAllQueued；本层「已入队」时机不可观测：
///      内核缓冲→池队列的提交受接收线程 poll 粒度影响）
TEST(UdpTest, StopDrainsQueuedTasks)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    constexpr int kCount = 20;
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);

    std::atomic<int> executed{0};
    server.setMessageCallback([&](const std::shared_ptr<Buffer>&, const Address&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));   // 拖慢工作者
        executed.fetch_add(1);
    });
    server.Start();

    Socket client(Socket::IPV4, Socket::NIO_UDP);
    ASSERT_TRUE(client.isAvailable());
    for (int i = 0; i < kCount; ++i)
    {
        SendDatagram(client, server.localAddr(), "load");
    }
    ASSERT_TRUE(WaitFor([&]() { return executed.load() == kCount; },
                        std::chrono::milliseconds(5000)));

    server.Stop();   // 排空路径回归（队列此时已空，Stop 干净收尾）
    EXPECT_EQ(executed.load(), kCount);
}

/// [界] 未 Stop 直接析构：自动收尾不挂死
TEST(UdpTest, DestructWithoutStopIsSafe)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    {
        UdpServer server("127.0.0.1:" + std::to_string(port), 2);
        server.setMessageCallback([](const std::shared_ptr<Buffer>&, const Address&) {});
        server.Start();
    }   // dtor：未 Stop → 自动收尾
    SUCCEED();
}

/// [界] W1.1：未 Start 直接析构——安全（构造仅创建未启动，无线程在跑）
TEST(UdpTest, DestructWithoutStartIsSafe)
{
    const uint16_t port = GetFreePort();   // M12：与其余用例一致的端口断言
    ASSERT_GT(port, 0);
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);
}   // dtor：m_started=false 跳过 Stop；Thread/ThreadPool 未启动态析构安全

/// [界] 审查 #3：未 Start 先 Stop——置停即返回（无线程可 join），幂等，析构安全
TEST(UdpTest, StopWithoutStartIsSafe)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);
    server.Stop();
    server.Stop();   // 幂等
}

} // namespace test
} // namespace bus
