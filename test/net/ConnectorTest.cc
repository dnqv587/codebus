//***********************************************
// name      : ConnectorTest.cc
// brief     : net 模块单元测试：被测 TcpClient.h 内的 Connector（非阻塞连接器）
// attention : 成功路径的 fd 由用例接管关闭（所有权移交语义）；
//             Stop 须在 loop 线程同步完成后再析构（析构契约断言）
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/EventLoop.h"
#include "net/TcpClient.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace bus {
namespace test {

class ConnectorTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_loop_thread = std::make_unique<LoopThread>();
    }

    void TearDown() override {
        m_loop_thread.reset();
    }

    /// @brief 起临时监听 socket 作为连接目标
    /// @return {listen_fd, port}（用例负责 close）
    std::pair<socket_t, uint16_t> StartListening() {
        const socket_t fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        EXPECT_GT(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        EXPECT_EQ(::listen(fd, 16), 0);
        return {fd, ntohs(addr.sin_port)};
    }

    /// @brief 在指定端口起监听（Stop 取消验证用）
    socket_t ListenOn(uint16_t port) {
        const socket_t fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        EXPECT_GT(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(fd, 16), 0);
        return fd;
    }

    /// @brief base loop 宿主线程
    std::unique_ptr<LoopThread> m_loop_thread;
};

/// [正] 立即成功：对监听端口 Start → NewConnectionCallback 收到合法 fd
///      （用例接管并关闭）；随后同步 Stop 干净收尾
TEST_F(ConnectorTest, ConnectImmediateSuccess) {
    const auto [listen_fd, port] = StartListening();

    std::atomic<bool> got_fd{false};
    std::atomic<socket_t> handed{-1};
    auto connector = std::make_shared<Connector>(std::ref(*m_loop_thread->loop()), Address("127.0.0.1", port));
    connector->setNewConnectionCallback([&](socket_t fd) {
        handed.store(fd);
        got_fd.store(true);
    });
    connector->Start();

    EXPECT_TRUE(WaitFor([&]() { return got_fd.load(); }));
    EXPECT_GT(handed.load(), 0);  // fd 已移交（Connector 侧置 -1）

    RunInLoopSync(m_loop_thread->loop(), [&]() { connector->Stop(); });
    ::close(handed.load());  // fd 所有权归用例
    ::close(listen_fd);
}

/// [正] 连接被拒（未监听端口，未启用 retry）→ FailedCallback 恰一次，
///      fd 真实移交（>=0），且回调内调用 Stop() 安全（审查 #1 契约：
///      用户在失败回调内收尾不挂死、不 UAF——stopInLoop 经 QueueInLoop
///      排到分派轮外执行）
TEST_F(ConnectorTest, ConnectRefusedFiresFailed) {
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::atomic<int> failed{0};
    std::atomic<bool> stopped_in_callback{false};
    auto connector = std::make_shared<Connector>(std::ref(*m_loop_thread->loop()), Address("127.0.0.1", port));
    connector->setFailedCallback([&](socket_t fd) {
        EXPECT_GE(fd, 0);  // 真实 fd 移交（INFO #9：锁定移交语义）
        failed.fetch_add(1);
        connector->Stop();  // 回调内收尾（审查 #1 场景的可达形态）
        stopped_in_callback.store(true);
        ::close(fd);  // 失败 fd 已移交用例（U3：短命连接需真实 fd）
    });
    connector->Start();

    EXPECT_TRUE(WaitFor([&]() { return failed.load() == 1; }));
    EXPECT_TRUE(stopped_in_callback.load());
    RunInLoopSync(m_loop_thread->loop(), []() {});  // 排空队列（stopInLoop 已执行）
}

/// [正] 退避期内 Stop：重试被取消——之后起监听，观察窗口内不再成功连接
TEST_F(ConnectorTest, StopDuringBackoffCancelsRetry) {
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::atomic<bool> got_fd{false};
    auto connector = std::make_shared<Connector>(std::ref(*m_loop_thread->loop()), Address("127.0.0.1", port));
    connector->setNewConnectionCallback([&](socket_t) { got_fd.store(true); });
    connector->enableRetry();
    connector->Start();

    // 首次尝试失败（ECONNREFUSED）→ 进入 500ms 退避
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    RunInLoopSync(m_loop_thread->loop(), [&]() { connector->Stop(); });

    // 在原端口起监听：若退避定时器未被取消，下次重试（≤500ms 后）将连接
    // 成功并触发回调；已取消则观察窗口内无事发生
    const socket_t listen_fd = ListenOn(port);
    EXPECT_FALSE(WaitFor([&]() { return got_fd.load(); }, std::chrono::milliseconds(1500))) << "Stop 后不应再发起重试";

    RunInLoopSync(m_loop_thread->loop(), [&]() { connector->Stop(); });  // 幂等
    ::close(listen_fd);
}

/// [界] 未 Start 即 Stop/析构：安全空路径（fd=-1、无定时器宿主天然满足契约）
TEST_F(ConnectorTest, StopBeforeStartIsSafe) {
    auto connector = std::make_shared<Connector>(std::ref(*m_loop_thread->loop()), Address("127.0.0.1", GetFreePort()));
    connector->Stop();  // Stop 闭包持 shared_ptr：对象存活至 loop 侧执行完
    SUCCEED();
}

}  // namespace test
}  // namespace bus
