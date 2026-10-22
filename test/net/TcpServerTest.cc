//***********************************************
// name      : TcpServerTest.cc
// brief     : net 模块单元测试：被测 src/net/TcpServer.h/.cc（服务器装配与生命周期）
// attention : TcpServer 析构前必须 Stop 且 DoneCallback 已触发（连接表空契约）
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "exception/Exception.hpp"
#include "integration/EchoTestHelper.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/TCPConnection.h"
#include "net/TcpServer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

class TcpServerTest : public ::testing::Test
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

    /// @brief 在 base loop 上装配 echo 回调并启动服务器
    /// @attention 内含启动屏障：返回时 Listener 已就绪（Start 的装配是异步
    ///             投递，空任务的 RunInLoopSync 与其 FIFO 串行）
    void StartEchoServer(TcpServer& server, std::atomic<int>* established = nullptr)
    {
        server.setConnectionCallback([established](const TCPConnection::ptr& conn) {
            if (established != nullptr && conn->isConnected())
            {
                established->fetch_add(1);
            }
        });
        server.setMessageCallback([](const TCPConnection::ptr& conn, Buffer* buf) {
            conn->send(buf->PeekAll());
            buf->SkipRead(buf->ReadableSize());
        });
        server.Start();
        WaitForServerReady(server);
    }

    /// @brief 启动屏障：Start 后等待 startInLoop 执行完毕
    void WaitForServerReady(const TcpServer& server)
    {
        ASSERT_NE(m_loop_thread->loop(), nullptr);
        RunInLoopSync(m_loop_thread->loop(), []() {});   // FIFO：排在 startInLoop 之后
        (void)server;
    }
};

/// [界] 地址解析：无端口/端口越界/裸 IPv6 冒号/非法 IP 均抛 AddressError
TEST_F(TcpServerTest, InvalidListenAddressThrows)
{
    EventLoop* loop = m_loop_thread->loop();
    EXPECT_THROW(TcpServer server(loop, "127.0.0.1", 0), Exception::AddressError);
    EXPECT_THROW(TcpServer server(loop, "127.0.0.1:0", 0), Exception::AddressError);
    EXPECT_THROW(TcpServer server(loop, "127.0.0.1:99999", 0), Exception::AddressError);
    EXPECT_THROW(TcpServer server(loop, "::1:8080", 0), Exception::AddressError);
    EXPECT_THROW(TcpServer server(loop, "999.1.1.1:8080", 0), Exception::AddressError);
}

/// [正] 单 loop：Start 幂等、建连触发 conn_cb、echo 往返、对端断开出表、
///      Stop 后 DoneCallback 恰好一次
TEST_F(TcpServerTest, SingleLoopEchoLifecycle)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    std::atomic<int> established{0};
    std::atomic<bool> done{false};
    {
        TcpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0);
        StartEchoServer(server, &established);
        server.Start();   // 幂等：重复 Start 无效果

        {
            EchoClient client(port);
            ASSERT_TRUE(client.valid());
            ASSERT_TRUE(client.SendAll("ping", 4));
            char echo[4]{};
            ASSERT_TRUE(client.RecvExact(echo, 4));
            EXPECT_EQ(std::string_view(echo, 4), "ping");
            ASSERT_TRUE(WaitFor([&]() { return established.load() == 1; }));
        }   // 对端断开（fd 关闭）→ FIN → 连接出表
        EXPECT_TRUE(WaitFor([&]() {
            return RunInLoopSync(m_loop_thread->loop(),
                                 [&]() { return server.ConnectionCount() == 0; });
        }));

        server.Stop([&done]() { done.store(true); });
        ASSERT_TRUE(WaitFor([&done]() { return done.load(); }));
    }
    SUCCEED();   // 析构契约满足（已 Stop 且表空）
}

/// [正] 多 loop（thread_num=2）：4 客户端全建连且 echo；conn_cb 观察到的
///      ioLoop 恰好 2 个互异（round-robin 确定性）
TEST_F(TcpServerTest, MultiLoopDistributesConnections)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    std::atomic<int> established{0};
    std::mutex mutex;
    std::set<EventLoop*> conn_loops;
    std::atomic<bool> done{false};
    {
        TcpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 2);
        server.setConnectionCallback([&](const TCPConnection::ptr& conn) {
            if (conn->isConnected())
            {
                established.fetch_add(1);
                std::lock_guard<std::mutex> lock(mutex);
                conn_loops.insert(conn->loop());
            }
        });
        server.setMessageCallback([](const TCPConnection::ptr& conn, Buffer* buf) {
            conn->send(buf->PeekAll());
            buf->SkipRead(buf->ReadableSize());
        });
        server.Start();
        WaitForServerReady(server);

        std::vector<std::unique_ptr<EchoClient>> clients;
        for (int i = 0; i < 4; ++i)
        {
            clients.emplace_back(std::make_unique<EchoClient>(port));
            ASSERT_TRUE(clients.back()->valid());
        }
        for (int i = 0; i < 4; ++i)
        {
            const std::string payload = "ping-" + std::to_string(i);
            ASSERT_TRUE(clients[i]->SendAll(payload.data(), payload.size()));
            std::vector<char> echo(payload.size());
            ASSERT_TRUE(clients[i]->RecvExact(echo.data(), echo.size()));
            EXPECT_EQ(std::string(echo.data(), echo.size()), payload);
        }
        ASSERT_TRUE(WaitFor([&]() { return established.load() == 4; }));
        {
            std::lock_guard<std::mutex> lock(mutex);
            EXPECT_EQ(conn_loops.size(), 2U);   // round-robin：4 连接落 2 个 ioLoop
        }

        server.Stop([&done]() { done.store(true); });
        ASSERT_TRUE(WaitFor([&done]() { return done.load(); }));
    }
    SUCCEED();
}

/// [正] Stop 全停：3 连接下 Stop → DoneCallback 一次触发、表空、新连接被拒
TEST_F(TcpServerTest, StopClosesAllAndInvokesDoneCallback)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    std::atomic<int> established{0};
    std::atomic<int> done_calls{0};
    {
        TcpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0);
        StartEchoServer(server, &established);

        std::vector<std::unique_ptr<EchoClient>> clients;
        for (int i = 0; i < 3; ++i)
        {
            clients.emplace_back(std::make_unique<EchoClient>(port));
            ASSERT_TRUE(clients.back()->valid());
        }
        ASSERT_TRUE(WaitFor([&]() { return established.load() == 3; }));
        clients.clear();   // 对端全部关闭（close 链先行排空，Stop 立即收尾）

        server.Stop([&done_calls]() { done_calls.fetch_add(1); });
        ASSERT_TRUE(WaitFor([&]() { return done_calls.load() == 1; }));
        // 完成后表空、监听已摘（新连接被拒）
        EXPECT_TRUE(RunInLoopSync(m_loop_thread->loop(),
                                  [&]() { return server.ConnectionCount() == 0; }));
        server.Stop([&done_calls]() { done_calls.fetch_add(1); });   // 二次 Stop 幂等
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(done_calls.load(), 1);
        EXPECT_FALSE(EchoClient(port, 1000).valid());
    }
    SUCCEED();
}

/// [正] 对端主动断开：连接出表且 DoneCallback 不触发（非停机路径不收尾）
TEST_F(TcpServerTest, PeerCloseRemovesConnectionWithoutStop)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    std::atomic<int> established{0};
    std::atomic<int> done_calls{0};
    {
        TcpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0);
        StartEchoServer(server, &established);

        EchoClient keep(port);
        std::optional<EchoClient> brief(std::in_place, port);
        ASSERT_TRUE(keep.valid() && brief->valid());
        ASSERT_TRUE(WaitFor([&]() { return established.load() == 2; }));

        brief.reset();   // 关闭其中一个：表内剩 1，DoneCallback 不触发
        EXPECT_TRUE(WaitFor([&]() {
            return RunInLoopSync(m_loop_thread->loop(),
                                 [&]() { return server.ConnectionCount() == 1; });
        }));
        EXPECT_EQ(done_calls.load(), 0);

        server.Stop([&done_calls]() { done_calls.fetch_add(1); });
        ASSERT_TRUE(WaitFor([&]() { return done_calls.load() == 1; }));
    }
    SUCCEED();
}

/// [界] 未 Start 即析构：安全空路径
TEST_F(TcpServerTest, DestructWithoutStartIsSafe)
{
    const uint16_t port = GetFreePort();
    {
        TcpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0);
    }
    SUCCEED();
}

} // namespace test
} // namespace bus
