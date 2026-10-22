//***********************************************
// name      : ListenerTest.cc
// brief     : net 模块单元测试：被测 src/net/Listener.h/.cc（accept 监听器回调与 fd 管理）
// attention : Listener 的构造/Listen/销毁必须在 loop 线程执行
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "integration/EchoTestHelper.h"
#include "net/Address.h"
#include "net/EventLoop.h"
#include "net/Listener.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

class ListenerTest : public ::testing::Test
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

TEST_F(ListenerTest, AcceptSingleConnection)
{
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::mutex mutex;
    std::vector<std::pair<socket_t, Address>> accepted;

    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address address) {
            std::lock_guard<std::mutex> lock(mutex);
            accepted.emplace_back(fd, std::move(address));
        });
        listener->Listen();
    });

    EchoClient client(port);
    ASSERT_TRUE(client.valid());

    ASSERT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted.size() == 1;
    }));

    {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_EQ(accepted[0].second.ipString(), "127.0.0.1");
        EXPECT_GT(accepted[0].second.port(), 0);
        EXPECT_GT(accepted[0].first, 0);
    }

    RunInLoopSync(loop, [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [fd, address] : accepted)
        {
            (void)address;
            ::close(fd);
        }
        accepted.clear();
        delete listener;
    });
}

TEST_F(ListenerTest, ConcurrentAccept)
{
    constexpr int kClientThreads = 8;
    constexpr int kClientsPerThread = 8;
    constexpr int kExpected = kClientThreads * kClientsPerThread;

    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::mutex mutex;
    std::vector<socket_t> accepted;
    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address) {
            std::lock_guard<std::mutex> lock(mutex);
            accepted.push_back(fd);
        });
        listener->Listen();
    });

    std::vector<std::thread> clients;
    clients.reserve(kClientThreads);
    for (int t = 0; t < kClientThreads; ++t)
    {
        clients.emplace_back([port]() {
            for (int i = 0; i < kClientsPerThread; ++i)
            {
                EchoClient client(port, 3000);
                ASSERT_TRUE(client.valid());
            }
        });
    }
    for (auto& thread : clients)
    {
        thread.join();
    }

    ASSERT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted.size() == static_cast<size_t>(kExpected);
    })) << "accepted=" << accepted.size();

    RunInLoopSync(loop, [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        // 校验 fd 唯一
        std::vector<socket_t> sorted = accepted;
        std::sort(sorted.begin(), sorted.end());
        EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end());

        for (socket_t fd : accepted)
        {
            ::close(fd);
        }
        accepted.clear();
        delete listener;
    });
}

/// [命] 析构释放 fd：50 次创建/Listen/销毁 fd 零净增（Debug 下安全销毁）
TEST_F(ListenerTest, DestructorReleasesFd)
{
    EventLoop* loop = m_loop_thread->loop();
    const int before = RunInLoopSync(loop, []() { return FdCount(); });
    ASSERT_GT(before, 0);

    RunInLoopSync(loop, [loop]() {
        for (int i = 0; i < 50; ++i)
        {
            const uint16_t port = GetFreePort();
            ASSERT_GT(port, 0);
            Listener listener(std::ref(*loop), Address("127.0.0.1", port));
            listener.Listen();
        }
    });

    const int after = RunInLoopSync(loop, []() { return FdCount(); });
    EXPECT_LE(after, before + 1) << "before=" << before << " after=" << after;
}

TEST_F(ListenerTest, NoCallbackClosesAcceptedConnection)
{
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->Listen(); // 不设置新连接回调
    });

    EchoClient client(port, 1000);
    ASSERT_TRUE(client.valid());

    char byte = 0;
    // 服务器未处理连接时会主动关闭，客户端读取应为对端关闭或超时失败
    EXPECT_FALSE(client.RecvExact(&byte, 1));

    RunInLoopSync(loop, [listener]() { delete listener; });
}

// ---------------------------------------------------------------------------
// 新增场景（P4）
// ---------------------------------------------------------------------------

/// [压] accept 风暴加深：16 线程 × 16 客户端 + fd 唯一性校验
TEST_F(ListenerTest, AcceptStorm16x16)
{
    constexpr int kClientThreads = 16;
    constexpr int kClientsPerThread = 16;
    constexpr int kExpected = kClientThreads * kClientsPerThread;

    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::mutex mutex;
    std::vector<socket_t> accepted;
    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address) {
            std::lock_guard<std::mutex> lock(mutex);
            accepted.push_back(fd);
        });
        listener->Listen();
    });

    std::vector<std::thread> clients;
    clients.reserve(kClientThreads);
    for (int t = 0; t < kClientThreads; ++t)
    {
        clients.emplace_back([port]() {
            for (int i = 0; i < kClientsPerThread; ++i)
            {
                EchoClient client(port, 5000);
                ASSERT_TRUE(client.valid());
            }
        });
    }
    for (auto& thread : clients)
    {
        thread.join();
    }

    ASSERT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted.size() == static_cast<size_t>(kExpected);
    }, std::chrono::milliseconds(10000))) << "accepted=" << accepted.size();

    RunInLoopSync(loop, [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<socket_t> sorted = accepted;
        std::sort(sorted.begin(), sorted.end());
        EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end());

        for (socket_t fd : accepted)
        {
            ::close(fd);
        }
        accepted.clear();
        delete listener;
    });
}

// TODO(Q14): Listen() 二次调用走 listen()+EnableReadEvent() 幂等路径，
// 不报错也不影响后续 accept（src/net/Listener.cc:30-35，返回值未检查）。
// 本用例锁定当前行为；若确认为应显式拒绝/告警，修复后改为正向断言。
TEST_F(ListenerTest, ListenTwiceIsHarmless)
{
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::mutex mutex;
    int accepted_count = 0;
    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address) {
            std::lock_guard<std::mutex> lock(mutex);
            ++accepted_count;
            ::close(fd);
        });
        listener->Listen();
        listener->Listen(); // 二次调用
    });

    EchoClient client(port);
    ASSERT_TRUE(client.valid());
    EXPECT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted_count == 1;
    }));

    RunInLoopSync(loop, [listener]() { delete listener; });
}

// TODO(Q14): 端口被占用时构造 Listener 不抛异常——bind 失败仅被 Socket 内部
// 记日志（src/net/Listener.cc:19 构造期 bind，Socket.cc:71-82 仅 LOG_ERROR），
// Listen() 返回 void 无法感知失败，客户端连接将被拒绝。本用例锁定当前行为；
// 若确认应向上抛出 AddressError/SocketError，修复后改为 EXPECT_THROW。
TEST_F(ListenerTest, BindOccupiedPortIsSilent)
{
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Listener* first = nullptr;
    Listener* second = nullptr;
    RunInLoopSync(loop, [&]() {
        first = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        first->Listen();

        EXPECT_NO_THROW(second = new Listener(std::ref(*loop), Address("127.0.0.1", port)));
        second->Listen(); // 占用端口的第二个 Listener：静默失败
    });

    // 第一个 Listener 仍正常工作
    std::mutex mutex;
    int accepted_count = 0;
    RunInLoopSync(loop, [&]() {
        first->setNewConnectionCallback([&](socket_t fd, Address) {
            std::lock_guard<std::mutex> lock(mutex);
            ++accepted_count;
            ::close(fd);
        });
    });

    EchoClient client(port, 1000);
    ASSERT_TRUE(client.valid());
    EXPECT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted_count == 1;
    }));

    RunInLoopSync(loop, [first, second]() { delete first; delete second; });
}

} // namespace test
} // namespace bus
