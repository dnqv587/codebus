//***********************************************
// name      : TcpClientTest.cc
// brief     : net 模块单元测试：被测 src/net/TcpClient.h/.cc（客户端装配与
//             生命周期，对 TcpServer 装配的真实服务端栈对测）
// attention : 每用例须满足析构契约：client 先 stop 且收到过 Disconnected、
//             server 先 Stop 等 DoneCallback 再出作用域
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/EventLoop.h"
#include "net/TCPConnection.h"
#include "net/TcpClient.h"
#include "net/TcpServer.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace bus
{
namespace test
{

class TcpClientTest : public ::testing::Test
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

    /// @brief 在指定端口装配并启动 echo 服务端（含启动屏障）
    void StartEchoServer(TcpServer& server, uint16_t port,
                         std::atomic<int>* established = nullptr,
                         std::atomic<int>* closed = nullptr)
    {
        server.setConnectionCallback([established, closed](const TCPConnection::ptr& conn) {
            if (conn->isConnected())
            {
                if (established != nullptr)
                {
                    established->fetch_add(1);
                }
            }
            else
            {
                if (closed != nullptr)
                {
                    closed->fetch_add(1);
                }
            }
        });
        server.setMessageCallback([](const TCPConnection::ptr& conn, Buffer* buf) {
            conn->send(buf->PeekAll());
            buf->SkipRead(buf->ReadableSize());
        });
        server.Start();
        RunInLoopSync(m_loop_thread->loop(), []() {});   // FIFO：排在 startInLoop 后
    }

    /// @brief 服务端契约收尾：Stop 并等 DoneCallback
    void ShutdownServer(TcpServer& server)
    {
        std::atomic<bool> done{false};
        server.Stop([&done]() { done.store(true); });
        ASSERT_TRUE(WaitFor([&done]() { return done.load(); }));
    }

    /// @brief 客户端回调装配（记录建连/断开与回显数据）
    struct ClientProbe
    {
        std::atomic<int> connected{0};
        std::atomic<bool> closed{false};
        std::atomic<bool> got_echo{false};
        std::string echoed;   // loop 线程写、got_echo 置位后测试线程读
    };

    void AttachClientProbe(TcpClient& client, ClientProbe& probe)
    {
        client.setConnectionCallback([&probe](const TCPConnection::ptr& conn) {
            if (conn->isConnected())
            {
                probe.connected.fetch_add(1);
            }
            else
            {
                probe.closed.store(true);
            }
        });
        client.setMessageCallback([&probe](const TCPConnection::ptr& conn, Buffer* buf) {
            probe.echoed.assign(buf->PeekAll().toString());
            buf->SkipRead(buf->ReadableSize());
            probe.got_echo.store(true);
        });
    }

    /// @brief base loop 宿主线程
    std::unique_ptr<LoopThread> m_loop_thread;
};

/// [正] 连接建立：conn_cb(Connected) 恰一次、connection() 非空且 Connected、
///      跨线程 send → msg_cb 收到原样数据
TEST_F(TcpClientTest, ConnectAndEcho)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    const std::string addr = "127.0.0.1:" + std::to_string(port);

    TcpServer server(m_loop_thread->loop(), addr, 0);
    std::atomic<int> server_established{0};
    StartEchoServer(server, port, &server_established);

    TcpClient client(m_loop_thread->loop(), addr, "echo-client");
    ClientProbe probe;
    AttachClientProbe(client, probe);
    client.connect();

    EXPECT_TRUE(WaitFor([&probe]() { return probe.connected.load() == 1; }));
    ASSERT_NE(client.connection(), nullptr);
    EXPECT_TRUE(client.connection()->isConnected());
    EXPECT_EQ(server_established.load(), 1);

    client.connection()->send("ping", 4);   // 测试线程（跨线程排队路径）
    EXPECT_TRUE(WaitFor([&probe]() { return probe.got_echo.load(); }));
    EXPECT_EQ(probe.echoed, "ping");

    ShutdownServer(server);
    EXPECT_TRUE(WaitFor([&probe]() { return probe.closed.load(); }));
    EXPECT_EQ(client.connection(), nullptr);
    client.stop();   // 已断开：仅置停（析构契约满足）
}

/// [正] 服务端关闭：server Stop → 客户端 conn_cb(Disconnected)、连接清空
TEST_F(TcpClientTest, ServerCloseTriggersClientClose)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    const std::string addr = "127.0.0.1:" + std::to_string(port);

    TcpServer server(m_loop_thread->loop(), addr, 0);
    StartEchoServer(server, port);
    TcpClient client(m_loop_thread->loop(), addr, "close-client");
    ClientProbe probe;
    AttachClientProbe(client, probe);
    client.connect();
    EXPECT_TRUE(WaitFor([&probe]() { return probe.connected.load() == 1; }));

    ShutdownServer(server);
    EXPECT_TRUE(WaitFor([&probe]() { return probe.closed.load(); }));
    EXPECT_EQ(client.connection(), nullptr);
    client.stop();
}

/// [正] 客户端半关：disconnect()（shutdown）→ 服务端收 EOF 关闭 →
///      客户端收到 Disconnected（双向闭环对拍）
TEST_F(TcpClientTest, ClientDisconnectHalfClose)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    const std::string addr = "127.0.0.1:" + std::to_string(port);

    TcpServer server(m_loop_thread->loop(), addr, 0);
    std::atomic<int> server_established{0};
    std::atomic<int> server_closed{0};
    StartEchoServer(server, port, &server_established, &server_closed);

    TcpClient client(m_loop_thread->loop(), addr, "half-close-client");
    ClientProbe probe;
    AttachClientProbe(client, probe);
    client.connect();
    EXPECT_TRUE(WaitFor([&probe]() { return probe.connected.load() == 1; }));

    client.disconnect();   // 优雅半关：服务端读 EOF → 关闭 → 客户端收 FIN
    EXPECT_TRUE(WaitFor([&server_closed]() { return server_closed.load() == 1; }));
    EXPECT_TRUE(WaitFor([&probe]() { return probe.closed.load(); }));
    EXPECT_EQ(client.connection(), nullptr);
    client.stop();
    ShutdownServer(server);   // 无连接：立即收尾
}

/// [正] 连接被拒（未监听端口，!retry）：conn_cb 收到 Disconnected 态短命连接
///      （统一失败通知语义），connection() 保持为空
TEST_F(TcpClientTest, ConnectRefusedNotifiesViaConnCb)
{
    const uint16_t port = GetFreePort();   // 无服务端
    ASSERT_GT(port, 0);

    std::atomic<int> notify{0};
    std::atomic<TCPConnection::Status> observed{TCPConnection::Disconnected};
    TcpClient client(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), "refused");
    client.setConnectionCallback([&](const TCPConnection::ptr& conn) {
        observed.store(conn->status());
        notify.fetch_add(1);
    });
    client.connect();

    EXPECT_TRUE(WaitFor([&notify]() { return notify.load() == 1; }));
    EXPECT_EQ(observed.load(), TCPConnection::Disconnected);   // 短命连接终态
    EXPECT_EQ(client.connection(), nullptr);
    client.stop();
}

/// [正] 重连：enableRetry + 首连失败（退避 500ms 窗口内起服务端）→
///      自动重连成功并完成 echo 往返
TEST_F(TcpClientTest, RetryReconnectsAfterServerStarts)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    const std::string addr = "127.0.0.1:" + std::to_string(port);

    TcpClient client(m_loop_thread->loop(), addr, "retry-client");
    ClientProbe probe;
    AttachClientProbe(client, probe);
    client.enableRetry();
    client.connect();   // 首连失败（无服务端）→ 进入 500ms 退避

    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // 处于退避窗口
    TcpServer server(m_loop_thread->loop(), addr, 0);
    StartEchoServer(server, port);

    EXPECT_TRUE(WaitFor([&probe]() { return probe.connected.load() == 1; },
                        std::chrono::milliseconds(5000)))
        << "退避期内服务端就绪后应自动重连";
    ASSERT_NE(client.connection(), nullptr);
    client.connection()->send("again", 5);
    EXPECT_TRUE(WaitFor([&probe]() { return probe.got_echo.load(); }));
    EXPECT_EQ(probe.echoed, "again");

    ShutdownServer(server);
    EXPECT_TRUE(WaitFor([&probe]() { return probe.closed.load(); }));
    client.stop();
}

/// [正] 退避期内 stop：取消重试——之后起服务端，观察窗口内不再建连
TEST_F(TcpClientTest, StopCancelsRetry)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    const std::string addr = "127.0.0.1:" + std::to_string(port);

    TcpClient client(m_loop_thread->loop(), addr, "stop-client");
    ClientProbe probe;
    AttachClientProbe(client, probe);
    client.enableRetry();
    client.connect();   // 首连失败 → 退避

    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // 首次失败已发生
    client.stop();      // 取消重试

    TcpServer server(m_loop_thread->loop(), addr, 0);
    StartEchoServer(server, port);
    EXPECT_FALSE(WaitFor([&probe]() { return probe.connected.load() >= 1; },
                         std::chrono::milliseconds(1500)))
        << "stop 后不应再发起重连";
    EXPECT_EQ(client.connection(), nullptr);

    ShutdownServer(server);
    // client 析构契约：stopped 且无连接 ✓
}

} // namespace test
} // namespace bus
