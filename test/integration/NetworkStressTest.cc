//***********************************************
// name      : NetworkStressTest.cc
// brief     : 集成压测：Echo 数据通路、连接风暴、背压、半关闭、全双工、
//             定时器交织、长时浸泡、多 loop 客户端（高并发线程安全/内存安全专项）
// attention : 使用回环地址与动态端口，不依赖外网；异步等待均带超时；
//             EchoServer 为 TcpServer 装配（单 loop）——本目标即产品装配
//             路径的集成验证；历史 D13/D18/D27 绕行与 D6 有意泄漏均已闭环
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "Timer.h"
#include "integration/EchoTestHelper.h"
#include "net/EventLoop.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

namespace
{

/// @brief 构造确定性消息（前 4 字节序号，后 4 字节客户端 id，其余为模式字节）
std::vector<char> MakeMessage(int client_id, int index, size_t size)
{
    std::vector<char> data(size, 0);
    if (size >= sizeof(uint32_t))
    {
        const uint32_t idx = static_cast<uint32_t>(index);
        std::memcpy(data.data(), &idx, sizeof(idx));
    }
    if (size >= 2 * sizeof(uint32_t))
    {
        const uint32_t cid = static_cast<uint32_t>(client_id);
        std::memcpy(data.data() + sizeof(uint32_t), &cid, sizeof(cid));
    }
    for (size_t i = 2 * sizeof(uint32_t); i < size; ++i)
    {
        data[i] = static_cast<char>((index * 31 + client_id * 17 + static_cast<int>(i)) & 0xFF);
    }
    return data;
}

/// @brief 生成绝对偏移相关的模式字节（用于大块/流水线数据）
char PatternByte(int client_id, size_t offset)
{
    return static_cast<char>((client_id * 17 + offset * 31) & 0xFF);
}

} // namespace

class NetworkStressTest : public ::testing::Test
{
 protected:
    void SetUp() override
    {
        m_loop_thread = std::make_unique<LoopThread>();
        m_port = GetFreePort();
        ASSERT_GT(m_port, 0);

        m_server = std::make_unique<EchoServer>(m_loop_thread->loop(), m_port);
        RunInLoopSync(m_loop_thread->loop(), [this]() { m_server->Start(); });

        m_fd_baseline = RunInLoopSync(m_loop_thread->loop(), []() { return FdCount(); });
        ASSERT_GT(m_fd_baseline, 0);
    }

    void TearDown() override
    {
        RunInLoopSync(m_loop_thread->loop(), [this]() { m_server->Shutdown(); });

        const bool no_leak = WaitFor(
            [this]() {
                return RunInLoopSync(m_loop_thread->loop(), []() { return FdCount(); }) <=
                       m_fd_baseline + 2;
            },
            std::chrono::milliseconds(5000));
        EXPECT_TRUE(no_leak) << "fd leak detected: baseline=" << m_fd_baseline;

        m_server.reset();
        m_loop_thread.reset();
    }

    /// @brief 运行事件循环的线程
    std::unique_ptr<LoopThread> m_loop_thread;
    /// @brief 服务器端口
    uint16_t m_port{0};
    /// @brief Echo 服务器
    std::unique_ptr<EchoServer> m_server;
    /// @brief 服务器启动后的 fd 基线
    int m_fd_baseline{0};
};

TEST_F(NetworkStressTest, EchoSingleClient)
{
    EchoClient client(m_port);
    ASSERT_TRUE(client.valid());

    for (int i = 0; i < 100; ++i)
    {
        const size_t size = 16 + static_cast<size_t>((i * 37) % 64);
        const std::vector<char> message = MakeMessage(0, i, size);
        ASSERT_TRUE(client.SendAll(message.data(), message.size()));

        std::vector<char> echo(size);
        ASSERT_TRUE(client.RecvExact(echo.data(), echo.size()));
        EXPECT_EQ(echo, message) << "index=" << i;
    }

    ASSERT_TRUE(WaitFor([&]() { return m_server->AcceptedCount() == 1; }));
    EXPECT_GT(m_server->EchoedBytes(), 0);
}

TEST_F(NetworkStressTest, EchoConcurrentClients)
{
    constexpr int kClientCount = 32;
    constexpr int kMessagesPerClient = 500;

    std::atomic<int> failures{0};
    std::vector<std::thread> clients;
    clients.reserve(kClientCount);
    for (int client_id = 0; client_id < kClientCount; ++client_id)
    {
        clients.emplace_back([this, client_id, &failures]() {
            EchoClient client(m_port, 10000);
            if (!client.valid())
            {
                failures.fetch_add(1);
                return;
            }
            for (int i = 0; i < kMessagesPerClient; ++i)
            {
                const size_t size = 16 + static_cast<size_t>((i * 53 + client_id * 97) % 4081);
                const std::vector<char> message = MakeMessage(client_id, i, size);
                if (!client.SendAll(message.data(), message.size()))
                {
                    failures.fetch_add(1);
                    return;
                }
                std::vector<char> echo(size);
                if (!client.RecvExact(echo.data(), echo.size()) || echo != message)
                {
                    failures.fetch_add(1);
                    return;
                }
            }
        });
    }
    for (auto& thread : clients)
    {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0);
    ASSERT_TRUE(WaitFor([&]() { return m_server->AcceptedCount() == kClientCount; }))
        << "accepted=" << m_server->AcceptedCount();
}

TEST_F(NetworkStressTest, EchoLargePayload)
{
    constexpr size_t kSize = 1024 * 1024;
    std::vector<char> payload(kSize);
    for (size_t i = 0; i < kSize; ++i)
    {
        payload[i] = PatternByte(0, i);
    }

    EchoClient client(m_port, 15000);
    ASSERT_TRUE(client.valid());
    ASSERT_TRUE(client.SendAll(payload.data(), payload.size()));

    std::vector<char> echo(kSize);
    ASSERT_TRUE(client.RecvExact(echo.data(), echo.size()));
    EXPECT_TRUE(echo == payload);

    ASSERT_TRUE(WaitFor([&]() { return m_server->AcceptedCount() == 1; }));
}

TEST_F(NetworkStressTest, ConnectionChurn)
{
    constexpr int kThreads = 8;
    constexpr int kClientsPerThread = 25;
    constexpr int kExpected = kThreads * kClientsPerThread;

    std::atomic<int> failures{0};
    std::vector<std::thread> clients;
    clients.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        clients.emplace_back([this, &failures]() {
            for (int i = 0; i < kClientsPerThread; ++i)
            {
                EchoClient client(m_port, 5000);
                if (!client.valid())
                {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : clients)
    {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0);
    ASSERT_TRUE(WaitFor([&]() { return m_server->AcceptedCount() == kExpected; }, std::chrono::milliseconds(10000)))
        << "accepted=" << m_server->AcceptedCount();

    // 连接 fd 在 Shutdown 时统一回收，由 TearDown 做 fd 泄漏断言
}

TEST_F(NetworkStressTest, HalfClose)
{
    EchoClient client(m_port, 5000);
    ASSERT_TRUE(client.valid());

    const char message[] = "bye";
    ASSERT_TRUE(client.SendAll(message, sizeof(message) - 1));
    client.ShutdownWrite();

    std::vector<char> echo(sizeof(message) - 1);
    ASSERT_TRUE(client.RecvExact(echo.data(), echo.size()));
    EXPECT_EQ(std::string(echo.data(), echo.size()), "bye");

    // 服务器正确语义（D27 闭环后连接即时销毁）：回显完成后收到对端 EOF →
    // 关闭连接发出 FIN → 客户端 poll 可读且 recv 返回 0（EOF 而非多余数据）
    pollfd pfd{client.fd(), POLLIN, 0};
    const int ready = ::poll(&pfd, 1, 2000);
    ASSERT_EQ(ready, 1) << "服务器应在回显后关闭并发 FIN";
    char extra[8]{};
    EXPECT_EQ(::recv(client.fd(), extra, sizeof(extra), MSG_DONTWAIT), 0)
        << "应为 EOF 而非多余数据";

    ASSERT_TRUE(WaitFor([&]() { return m_server->AcceptedCount() == 1; }));
}

TEST_F(NetworkStressTest, PipelinedMessages)
{
    constexpr int kClientCount = 8;
    constexpr int kMessagesPerClient = 200;

    std::atomic<int> failures{0};
    std::vector<std::thread> clients;
    clients.reserve(kClientCount);
    for (int client_id = 0; client_id < kClientCount; ++client_id)
    {
        clients.emplace_back([this, client_id, &failures]() {
            EchoClient client(m_port, 15000);
            if (!client.valid())
            {
                failures.fetch_add(1);
                return;
            }

            std::vector<size_t> sizes;
            sizes.reserve(kMessagesPerClient);
            for (int i = 0; i < kMessagesPerClient; ++i)
            {
                const size_t size = 16 + static_cast<size_t>((i * 29 + client_id) % 241);
                sizes.push_back(size);
                const std::vector<char> message = MakeMessage(client_id, i, size);
                if (!client.SendAll(message.data(), message.size()))
                {
                    failures.fetch_add(1);
                    return;
                }
            }

            for (int i = 0; i < kMessagesPerClient; ++i)
            {
                std::vector<char> echo(sizes[static_cast<size_t>(i)]);
                if (!client.RecvExact(echo.data(), echo.size()) ||
                    echo != MakeMessage(client_id, i, echo.size()))
                {
                    failures.fetch_add(1);
                    return;
                }
            }
        });
    }
    for (auto& thread : clients)
    {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0);
    ASSERT_TRUE(WaitFor([&]() { return m_server->AcceptedCount() == kClientCount; }));
}

TEST_F(NetworkStressTest, FullDuplexConcurrentReadWrite)
{
    constexpr int kClientCount = 16;
    constexpr size_t kBytesPerClient = 256 * 1024;

    std::atomic<int> failures{0};
    std::vector<std::thread> clients;
    clients.reserve(kClientCount);
    for (int client_id = 0; client_id < kClientCount; ++client_id)
    {
        clients.emplace_back([this, client_id, &failures]() {
            EchoClient client(m_port, 15000);
            if (!client.valid())
            {
                failures.fetch_add(1);
                return;
            }

            std::thread writer([&client, client_id, &failures]() {
                constexpr size_t kChunk = 4096;
                std::vector<char> buffer(kChunk);
                for (size_t offset = 0; offset < kBytesPerClient; offset += kChunk)
                {
                    const size_t size = std::min(kChunk, kBytesPerClient - offset);
                    for (size_t i = 0; i < size; ++i)
                    {
                        buffer[i] = PatternByte(client_id, offset + i);
                    }
                    if (!client.SendAll(buffer.data(), size))
                    {
                        failures.fetch_add(1);
                        return;
                    }
                }
            });

            constexpr size_t kReadChunk = 2048;
            std::vector<char> buffer(kReadChunk);
            size_t received = 0;
            while (received < kBytesPerClient)
            {
                const size_t size = std::min(kReadChunk, kBytesPerClient - received);
                if (!client.RecvExact(buffer.data(), size))
                {
                    failures.fetch_add(1);
                    break;
                }
                for (size_t i = 0; i < size; ++i)
                {
                    if (buffer[i] != PatternByte(client_id, received + i))
                    {
                        failures.fetch_add(1);
                        break;
                    }
                }
                received += size;
            }

            writer.join();
        });
    }
    for (auto& thread : clients)
    {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0);
    // 连接 fd 在 Shutdown 时统一回收，由 TearDown 做 fd 泄漏断言
}

// ---------------------------------------------------------------------------
// 新增场景（P11 高并发/内存安全专项）
// ---------------------------------------------------------------------------

/// [命] 连接风暴 + 未排空发送缓冲时断连：服务器侧输出队列持有路径的内存安全
/// （客户端在服务器仍待回显时关闭，服务器写侧遇 EPIPE/RST 走关闭路径）
TEST_F(NetworkStressTest, EchoChurnWithPendingSend)
{
    constexpr int kThreads = 4;
    constexpr int kClientsPerThread = 20;

    std::atomic<int> connected{0};
    std::vector<std::thread> clients;
    clients.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        clients.emplace_back([this, &connected]() {
            for (int i = 0; i < kClientsPerThread; ++i)
            {
                EchoClient client(m_port, 3000);
                if (!client.valid())
                {
                    continue;
                }
                connected.fetch_add(1);
                // 只写不读：制造服务器待回显积压后立即断连（RST 路径）
                std::vector<char> burst(64 * 1024, 'x');
                (void)client.SendAll(burst.data(), burst.size());
                // client 析构关闭 fd，服务器写侧将遇 EPIPE
            }
        });
    }
    for (auto& thread : clients)
    {
        thread.join();
    }

    EXPECT_GT(connected.load(), 0);
    // 服务器在积压+断连风暴后仍应存活并正确计数
    ASSERT_TRUE(WaitFor([&]() { return m_server->AcceptedCount() >= connected.load(); },
                        std::chrono::milliseconds(10000)))
        << "accepted=" << m_server->AcceptedCount() << " connected=" << connected.load();
    // fd 回收由 TearDown 断言（僵尸连接在 Shutdown 统一销毁）
}

/// [界] 大写小读不对称负载：服务器输出队列增长/消费路径（256KB 写、64B 粒度读）
TEST_F(NetworkStressTest, EchoAsymmetric)
{
    constexpr size_t kTotal = 256 * 1024;
    constexpr size_t kReadChunk = 64;

    EchoClient client(m_port, 15000);
    ASSERT_TRUE(client.valid());

    std::vector<char> payload(kTotal);
    for (size_t i = 0; i < kTotal; ++i)
    {
        payload[i] = PatternByte(3, i);
    }
    ASSERT_TRUE(client.SendAll(payload.data(), payload.size()));

    size_t received = 0;
    while (received < kTotal)
    {
        const size_t size = std::min(kReadChunk, kTotal - received);
        std::vector<char> echo(size);
        ASSERT_TRUE(client.RecvExact(echo.data(), echo.size()));
        for (size_t i = 0; i < size; ++i)
        {
            ASSERT_EQ(echo[i], PatternByte(3, received + i)) << "offset=" << received + i;
        }
        received += size;
    }
    SUCCEED();
}

/// [并] 定时器任务与 IO 回调同 loop 交织：Timer/Channel 共存的分派安全
TEST_F(NetworkStressTest, TimerAndIOInterleave)
{
    TimerManager* timers = RunInLoopSync(m_loop_thread->loop(), [this]() {
        return new TimerManager(std::ref(*m_loop_thread->loop()));
    });

    std::atomic<int> timer_ticks{0};
    RunInLoopSync(m_loop_thread->loop(), [&]() {
        timers->addTimer([&]() { timer_ticks.fetch_add(1); },
                         std::chrono::steady_clock::now() + std::chrono::milliseconds(10),
                         std::chrono::milliseconds(10));
    });

    // IO 流量与定时器并行 1 秒
    std::atomic<int> failures{0};
    std::thread io_thread([this, &failures]() {
        EchoClient client(m_port, 5000);
        if (!client.valid())
        {
            failures.fetch_add(1);
            return;
        }
        for (int i = 0; i < 200; ++i)
        {
            const std::vector<char> message = MakeMessage(9, i, 64);
            if (!client.SendAll(message.data(), message.size()))
            {
                failures.fetch_add(1);
                return;
            }
            std::vector<char> echo(64);
            if (!client.RecvExact(echo.data(), echo.size()) || echo != message)
            {
                failures.fetch_add(1);
                return;
            }
        }
    });
    io_thread.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_TRUE(WaitFor([&]() { return timer_ticks.load() > 10; }))
        << "ticks=" << timer_ticks.load();
    ASSERT_TRUE(WaitFor([&]() { return m_server->AcceptedCount() >= 1; }));

    // loop 线程析构（Channel 摘除的线程断言要求；D6 闭环后不再泄漏）
    RunInLoopSync(m_loop_thread->loop(), [timers]() { delete timers; });
}

/// [压] 长时浸泡：混合负载（echo + 连接风暴）持续运行，观察慢性泄漏/错误累积
/// @attention 浸泡时长取 5s（CI 时长权衡），TSAN/ASAN 构建下同样运行
TEST_F(NetworkStressTest, LongRunSoak)
{
    constexpr auto kDuration = std::chrono::seconds(5);
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::atomic<int> rounds{0};

    std::vector<std::thread> workers;
    workers.emplace_back([this, &stop, &failures]() {
        // 持续 echo 流
        EchoClient client(m_port, 5000);
        if (client.valid())
        {
            int i = 0;
            while (!stop.load())
            {
                const std::vector<char> message = MakeMessage(11, i, 128);
                if (!client.SendAll(message.data(), message.size()))
                {
                    failures.fetch_add(1);
                    break;
                }
                std::vector<char> echo(128);
                if (!client.RecvExact(echo.data(), echo.size()) || echo != message)
                {
                    failures.fetch_add(1);
                    break;
                }
                ++i;
            }
        }
        else
        {
            failures.fetch_add(1);
        }
    });
    workers.emplace_back([this, &stop, &rounds]() {
        // 持续连接风暴
        while (!stop.load())
        {
            EchoClient client(m_port, 2000);
            rounds.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    std::this_thread::sleep_for(kDuration);
    stop.store(true);
    for (auto& thread : workers)
    {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(rounds.load(), 10);           // 风暴侧确实跑了若干轮
    EXPECT_GT(m_server->AcceptedCount(), 10); // 累计接受数正常增长
}

/// [并] 多客户端 loop 压服务端单 loop：跨 loop 任务队列竞争
TEST_F(NetworkStressTest, MultiLoopClientStorm)
{
    constexpr int kExtraLoops = 3;
    constexpr int kSessionsPerLoop = 10;

    std::vector<std::unique_ptr<LoopThread>> client_loops;
    client_loops.reserve(kExtraLoops);
    for (int i = 0; i < kExtraLoops; ++i)
    {
        client_loops.push_back(std::make_unique<LoopThread>());
    }

    std::atomic<int> failures{0};
    std::atomic<int> sessions{0};
    for (auto& loop_thread : client_loops)
    {
        RunInLoopSync(loop_thread->loop(), [this, &failures, &sessions]() {
            // 在客户端 loop 线程内串行建立会话（跨 loop 并发压服务端单 loop）
            for (int i = 0; i < kSessionsPerLoop; ++i)
            {
                EchoClient client(m_port, 5000);
                if (!client.valid())
                {
                    failures.fetch_add(1);
                    continue;
                }
                sessions.fetch_add(1);
                const std::vector<char> message = MakeMessage(21, i, 256);
                if (!client.SendAll(message.data(), message.size()))
                {
                    failures.fetch_add(1);
                    continue;
                }
                std::vector<char> echo(256);
                if (!client.RecvExact(echo.data(), echo.size()) || echo != message)
                {
                    failures.fetch_add(1);
                }
            }
        });
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(sessions.load(), kExtraLoops * kSessionsPerLoop);
    ASSERT_TRUE(WaitFor(
        [&]() { return m_server->AcceptedCount() >= kExtraLoops * kSessionsPerLoop; },
        std::chrono::milliseconds(10000)))
        << "accepted=" << m_server->AcceptedCount();

    // RunInLoopSync 的 promise/future 已保证会话完成，客户端 loop 可安全停止
    for (auto& loop_thread : client_loops)
    {
        loop_thread.reset();
    }
}

} // namespace test
} // namespace bus
