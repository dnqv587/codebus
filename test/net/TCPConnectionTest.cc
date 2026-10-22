//***********************************************
// name      : TCPConnectionTest.cc
// brief     : net 模块单元测试：被测 src/net/TCPConnection.h/.cc（连接状态机、发送路径、回调）
// attention : 连接建立与 send 族路径均已激活（D9/D10/D11 已闭环：
//             ConnectEstablishedEcho/SendFromNonLoopThreadIsSafe/SendBuffer 覆盖）
//***********************************************
#include <gtest/gtest.h>
#include "Buffer.hpp"
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/Address.h"
#include "net/EventLoop.h"
#include "net/TCPConnection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <fcntl.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace bus
{
namespace test
{

class TCPConnectionTest : public ::testing::Test
{
 protected:
    void SetUp() override
    {
        m_loop_thread = std::make_unique<LoopThread>();
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, m_fds), 0) << strerror(errno);
        m_peer.reset(m_fds[1]);
    }

    void TearDown() override
    {
        if (m_connection)
        {
            RunInLoopSync(m_loop_thread->loop(), [this]() { m_connection.reset(); });
        }
        m_loop_thread.reset();
    }

    /// @brief 创建连接对象（在 loop 线程）
    void CreateConnection(std::string_view name = "test-conn")
    {
        EventLoop* loop = m_loop_thread->loop();
        RunInLoopSync(loop, [this, loop, name]() {
            m_connection = std::make_shared<TCPConnection>(
                std::ref(*loop), name, m_fds[0],
                Address("127.0.0.1", 12345), Address("127.0.0.1", 12346));
        });
    }

    /// @brief 运行事件循环的线程
    std::unique_ptr<LoopThread> m_loop_thread;
    /// @brief socketpair
    int m_fds[2]{-1, -1};
    /// @brief 对端 fd
    ScopedFd m_peer;
    /// @brief 被测连接
    std::shared_ptr<TCPConnection> m_connection;
};

TEST_F(TCPConnectionTest, ConstructAndDestruct)
{
    CreateConnection();

    EXPECT_EQ(m_connection->name(), "test-conn");
    EXPECT_EQ(m_connection->fd(), m_fds[0]);
    EXPECT_EQ(m_connection->status(), TCPConnection::Disconnected);
    EXPECT_EQ(m_connection->getStatus(), m_connection->status());
    EXPECT_FALSE(m_connection->isConnected());
    EXPECT_EQ(m_connection->localAddr().port(), 12345);
    EXPECT_EQ(m_connection->remoteAddr().port(), 12346);
    EXPECT_EQ(m_connection->loop(), m_loop_thread->loop());
    EXPECT_EQ(m_connection->getLoop(), m_connection->loop());

    // TearDown 在 loop 线程销毁（析构断言状态为 Disconnected）
}

TEST_F(TCPConnectionTest, CloseTransitionsToDisconnected)
{
    CreateConnection();
    std::atomic<bool> closed{false};

    EventLoop* loop = m_loop_thread->loop();
    RunInLoopSync(loop, [this, &closed]() {
        m_connection->setCloseCallback([&closed](const TCPConnection::ptr&) {
            closed.store(true);
        });
        m_connection->close();
    });

    EXPECT_TRUE(closed.load());
    EXPECT_EQ(m_connection->status(), TCPConnection::Disconnected);
    EXPECT_FALSE(m_connection->isConnected());
}

TEST_F(TCPConnectionTest, CallbacksCanBeSet)
{
    CreateConnection();
    EventLoop* loop = m_loop_thread->loop();

    RunInLoopSync(loop, [this]() {
        m_connection->setConnectionCallback([](const TCPConnection::ptr&) {});
        m_connection->setMessageCallback([](const TCPConnection::ptr&, Buffer*) {});
        m_connection->setWriteCompleteCallback([](const TCPConnection::ptr&) {});
        m_connection->setHighWaterMarkCallback([](const TCPConnection::ptr&, size_t) {}, 1024);
        m_connection->setTcpNoDelay(true); // 未连接状态下仅设置 socket 选项
    });

    SUCCEED();
}

TEST_F(TCPConnectionTest, SendIsIgnoredWhenDisconnected)
{
    CreateConnection();

    // 非阻塞读取对端，避免阻塞
    const int flags = ::fcntl(m_peer.get(), F_GETFL);
    ASSERT_GE(flags, 0);
    ASSERT_GE(::fcntl(m_peer.get(), F_SETFL, flags | O_NONBLOCK), 0);

    EventLoop* loop = m_loop_thread->loop();
    RunInLoopSync(loop, [this]() { m_connection->send("payload", 7); });

    char buffer[16]{};
    EXPECT_EQ(::read(m_peer.get(), buffer, sizeof(buffer)), static_cast<ssize_t>(-1));
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
}

/// [正] 非 loop 线程 send：数据拷贝入队，局部缓冲销毁后仍完整到达（D9）
TEST_F(TCPConnectionTest, SendFromNonLoopThreadIsSafe)
{
    CreateConnection();
    EventLoop* loop = m_loop_thread->loop();
    RunInLoopSync(loop, [this]() { m_connection->connectEstablished(); });

    {
        std::string payload(2048, 'x');
        m_connection->send(payload);          // 测试线程（非 loop），payload 随作用域销毁
    }

    std::vector<char> received(2048);
    size_t got = 0;
    while (got < received.size())            // 阻塞收满（本用例数据量一次写完；
                                             // 部分写→output buffer 路径见 EchoAsymmetric）
    {
        const ssize_t n = ::read(m_peer.get(), received.data() + got, received.size() - got);
        ASSERT_GT(n, 0);
        got += static_cast<size_t>(n);
    }
    EXPECT_EQ(std::string_view(received.data(), received.size()), std::string(2048, 'x'));

    RunInLoopSync(loop, [this]() { m_connection->close(); });
}

/// [正] send(Buffer*)：数据发出且调用方 Buffer 被消费（所有权转移语义，D10）
TEST_F(TCPConnectionTest, SendBuffer)
{
    CreateConnection();
    EventLoop* loop = m_loop_thread->loop();
    RunInLoopSync(loop, [this]() { m_connection->connectEstablished(); });

    Buffer buffer(16);
    buffer.Write(Slice("buffered"));
    m_connection->send(&buffer);
    EXPECT_EQ(buffer.ReadableSize(), 0U);    // 可读区已消费

    char echo[8]{};
    ASSERT_EQ(::read(m_peer.get(), echo, 8), 8);
    EXPECT_EQ(std::string_view(echo, 8), "buffered");

    RunInLoopSync(loop, [this]() { m_connection->close(); });
}

/// [正] connectEstablished：进入 Connected、触发连接回调、激活读路径并回显
TEST_F(TCPConnectionTest, ConnectEstablishedEcho)
{
    CreateConnection();
    EventLoop* loop = m_loop_thread->loop();
    std::atomic<bool> connected{false};
    std::atomic<bool> echoed{false};

    RunInLoopSync(loop, [this, &connected, &echoed]() {
        m_connection->setConnectionCallback([&connected](const TCPConnection::ptr&) {
            connected.store(true);
        });
        m_connection->setMessageCallback([this, &echoed](const TCPConnection::ptr&, Buffer* buf) {
            m_connection->send(buf->PeekAll());   // 回显（同线程 send 立即执行）
            buf->SkipRead(buf->ReadableSize());
            echoed.store(true);
        });
        m_connection->connectEstablished();
        EXPECT_EQ(m_connection->status(), TCPConnection::Connected);
    });
    EXPECT_TRUE(connected.load());
    EXPECT_TRUE(m_connection->isConnected());

    // 对端写入 -> 读事件激活 -> MessageCallback -> 回显
    ASSERT_EQ(::write(m_peer.get(), "ping", 4), 4);
    char echo[4]{};
    ASSERT_EQ(::read(m_peer.get(), echo, 4), 4);   // 阻塞等回显
    EXPECT_EQ(std::string_view(echo, 4), "ping");
    EXPECT_TRUE(WaitFor([&echoed]() { return echoed.load(); }));

    // Connected 析构会触发析构断言（~TCPConnection assert Disconnected），先关闭
    RunInLoopSync(loop, [this]() { m_connection->close(); });
    EXPECT_EQ(m_connection->status(), TCPConnection::Disconnected);
}

/// [正] 对端被动断开（FIN）：从 Connected 直入 handleClose，干净关闭（C1 回归：
///      D11 激活读路径后，原 assert(Disconnecting) 在被动关闭场景必然 SIGABRT）
TEST_F(TCPConnectionTest, PeerCloseTriggersHandleCloseFromConnected)
{
    CreateConnection();
    EventLoop* loop = m_loop_thread->loop();
    std::atomic<bool> closed{false};

    RunInLoopSync(loop, [this, &closed]() {
        m_connection->setCloseCallback([&closed](const TCPConnection::ptr&) {
            closed.store(true);
        });
        m_connection->connectEstablished();
    });

    m_peer.reset();   // 对端关闭 -> FIN -> 读事件返回 0 -> handleClose
    EXPECT_TRUE(WaitFor([&closed]() { return closed.load(); }));
    // status() 在 loop 线程读：close_cb 置位发生在 handleClose 尾部写 m_status
    // 之前，测试线程读存在 Q30 竞争窗口；RunInLoopSync 与 handleClose 串行
    const auto final_status = RunInLoopSync(loop, [this]() { return m_connection->status(); });
    EXPECT_EQ(final_status, TCPConnection::Disconnected);
}

// ---------------------------------------------------------------------------
// 新增场景（P4）
// ---------------------------------------------------------------------------

// TODO(Q15): 双 close 当前会二次触发 CloseCallback：第二次 close() 将状态从
// Disconnected 改回 Disconnecting 并再次排队 handleClose（src/net/TCPConnection.cc
// close/handleClose）。本用例锁定当前行为；若确认 close 应幂等（仅触发一次
// 回调），修复后改为 EXPECT_EQ(close_calls, 1)。
// 注：历史上二次 close 还会留下 events=0 的僵尸 epoll 注册，该副作用已随
// D18 修复（UpdateChannel 对 kNone 不再 ADD）消失。
TEST_F(TCPConnectionTest, DoubleCloseFiresCloseCallbackTwice)
{
    CreateConnection();
    std::atomic<int> close_calls{0};

    EventLoop* loop = m_loop_thread->loop();
    RunInLoopSync(loop, [this, &close_calls]() {
        m_connection->setCloseCallback([&close_calls](const TCPConnection::ptr&) {
            close_calls.fetch_add(1);
        });
        m_connection->close();
    });
    EXPECT_TRUE(WaitFor([&]() { return close_calls.load() >= 1; }));
    EXPECT_EQ(m_connection->status(), TCPConnection::Disconnected);

    RunInLoopSync(loop, [this]() { m_connection->close(); }); // 二次 close
    EXPECT_TRUE(WaitFor([&]() { return close_calls.load() >= 2; }));

    EXPECT_EQ(close_calls.load(), 2); // 现状：回调被再次触发
    EXPECT_EQ(m_connection->status(), TCPConnection::Disconnected);
}

} // namespace test
} // namespace bus
