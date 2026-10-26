//***********************************************
// name      : SocketTest.cc
// brief     : net 模块单元测试：被测 src/net/Socket.h/.cc（fd RAII、IO 读写、连接建立、选项）
// attention : 不依赖外网；Socket 析构会关闭 fd，禁止手动 close 后再次析构；
//             accept() 在阻塞 listen socket 上无连接时会挂起，相关用例使用
//             NIO_TCP 服务端；含 EXPECT_EXIT 死亡测试（需 Debug 构建），
//             sanitizer 构建下由守卫宏跳过
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "Buffer.hpp"
#include "net/Address.h"
#include "net/Socket.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <utility>

namespace bus
{
namespace test
{

namespace
{

/// @brief 创建一对本地 socket
std::pair<int, int> MakeSocketPair(int type = SOCK_STREAM)
{
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, type, 0, fds) < 0)
    {
        return {-1, -1};
    }
    return {fds[0], fds[1]};
}

} // namespace

TEST(SocketTest, PairReadWrite)
{
    auto [local_fd, peer_fd] = MakeSocketPair();
    ASSERT_GE(local_fd, 0);
    ASSERT_GE(peer_fd, 0);
    ScopedFd peer(peer_fd);
    Socket socket(local_fd);

    const char out[] = "hello";
    EXPECT_EQ(socket.write(out, sizeof(out) - 1), static_cast<ssize_t>(sizeof(out) - 1));

    std::array<char, 16> buffer{};
    EXPECT_EQ(::read(peer.get(), buffer.data(), buffer.size()),
              static_cast<ssize_t>(sizeof(out) - 1));
    EXPECT_EQ(std::string(buffer.data(), sizeof(out) - 1), "hello");

    const char reply[] = "world";
    EXPECT_EQ(::write(peer.get(), reply, sizeof(reply) - 1),
              static_cast<ssize_t>(sizeof(reply) - 1));

    std::array<char, 16> in_buffer{};
    EXPECT_EQ(socket.read(in_buffer.data(), in_buffer.size()),
              static_cast<ssize_t>(sizeof(reply) - 1));
    EXPECT_EQ(std::string(in_buffer.data(), sizeof(reply) - 1), "world");
}

TEST(SocketTest, VectoredIO)
{
    auto [local_fd, peer_fd] = MakeSocketPair();
    ASSERT_GE(local_fd, 0);
    ASSERT_GE(peer_fd, 0);
    ScopedFd peer(peer_fd);
    Socket socket(local_fd);

    std::array<char, 3> first{'a', 'b', 'c'};
    std::array<char, 3> second{'d', 'e', 'f'};
    iovec output[2] = {
        {first.data(), first.size()},
        {second.data(), second.size()},
    };
    EXPECT_EQ(socket.writev(output, 2), static_cast<ssize_t>(6));

    std::array<char, 6> received{};
    EXPECT_EQ(::read(peer.get(), received.data(), received.size()), static_cast<ssize_t>(6));
    EXPECT_EQ(std::string(received.data(), received.size()), "abcdef");

    ASSERT_EQ(::write(peer.get(), "uvwxyz", 6), static_cast<ssize_t>(6));

    std::array<char, 3> in_first{};
    std::array<char, 3> in_second{};
    iovec input[2] = {
        {in_first.data(), in_first.size()},
        {in_second.data(), in_second.size()},
    };
    EXPECT_EQ(socket.readv(input, 2), static_cast<ssize_t>(6));
    EXPECT_EQ(std::string(in_first.data(), in_first.size()), "uvw");
    EXPECT_EQ(std::string(in_second.data(), in_second.size()), "xyz");
}

TEST(SocketTest, ShutdownWrite)
{
    auto [local_fd, peer_fd] = MakeSocketPair();
    ASSERT_GE(local_fd, 0);
    ASSERT_GE(peer_fd, 0);
    ScopedFd peer(peer_fd);
    Socket socket(local_fd);

    EXPECT_TRUE(socket.ShutdownWrite());

    char buffer[8]{};
    EXPECT_EQ(::read(peer.get(), buffer, sizeof(buffer)), static_cast<ssize_t>(0));
}

TEST(SocketTest, NonBlockingReadReturnsEagain)
{
    auto [local_fd, peer_fd] = MakeSocketPair();
    ASSERT_GE(local_fd, 0);
    ASSERT_GE(peer_fd, 0);
    ScopedFd peer(peer_fd);
    Socket socket(local_fd);

    ASSERT_TRUE(socket.setNonBlockAndCloseOnExec());

    char buffer[8]{};
    EXPECT_EQ(socket.read(buffer, sizeof(buffer)), static_cast<ssize_t>(-1));
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
}

/// [正] setNonBlockAndCloseOnExec 置位 FD_CLOEXEC（经 F_SETFD）
TEST(SocketTest, SetNonBlockAndCloseOnExecSetsFdCloexec)
{
    auto [local_fd, peer_fd] = MakeSocketPair();
    ASSERT_GE(local_fd, 0);
    ASSERT_GE(peer_fd, 0);
    ScopedFd peer(peer_fd);
    Socket socket(local_fd);

    ASSERT_TRUE(socket.setNonBlockAndCloseOnExec());
    const int flags = ::fcntl(socket.fd(), F_GETFD);
    ASSERT_GE(flags, 0);
    EXPECT_TRUE((flags & FD_CLOEXEC) != 0);
}

TEST(SocketTest, TcpBindListenAcceptConnect)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Socket server(Socket::IPV4, Socket::TCP);
    ASSERT_GT(server.fd(), 0);
    EXPECT_TRUE(server.setReuseAddr(true));

    Address address("127.0.0.1", port);
    ASSERT_TRUE(server.bind(address));
    ASSERT_TRUE(server.listen());

    Socket client(Socket::IPV4, Socket::TCP);
    ASSERT_GT(client.fd(), 0);
    ASSERT_TRUE(client.connect(address));

    auto [accepted_fd, peer_address] = server.accept();
    ASSERT_GT(accepted_fd, 0);
    Socket accepted(accepted_fd);

    EXPECT_EQ(peer_address.ipString(), "127.0.0.1");

    const auto local = client.getLocalAddr();
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(peer_address.port(), local->port());

    const auto remote = accepted.getRemoteAddr();
    ASSERT_TRUE(remote.has_value());
    EXPECT_EQ(remote->port(), local->port());

    const char message[] = "ping";
    EXPECT_EQ(client.write(message, sizeof(message) - 1),
              static_cast<ssize_t>(sizeof(message) - 1));
    std::array<char, 8> buffer{};
    EXPECT_EQ(accepted.read(buffer.data(), buffer.size()),
              static_cast<ssize_t>(sizeof(message) - 1));
    EXPECT_EQ(std::string(buffer.data(), sizeof(message) - 1), "ping");
}

TEST(SocketTest, TcpOptions)
{
    Socket socket(Socket::IPV4, Socket::TCP);
    ASSERT_GT(socket.fd(), 0);

    EXPECT_TRUE(socket.setTcpNoDelay(true));
    EXPECT_TRUE(socket.setKeepAlive(true));
    EXPECT_TRUE(socket.setReuseAddr(true));
    EXPECT_TRUE(socket.setReusePort(true));
}

/// [错] getSocketErrno 报告 SO_ERROR 真实错误（连接拒绝场景）
TEST(SocketTest, GetSocketErrnoReportsSoError)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Socket client(Socket::IPV4, Socket::NIO_TCP);
    ASSERT_GT(client.fd(), 0);
    Address address("127.0.0.1", port);
    EXPECT_FALSE(client.connect(address)); // 非阻塞 connect 返回 EINPROGRESS
    EXPECT_EQ(client.getSocketErrno(), ECONNREFUSED);
}

// ---------------------------------------------------------------------------
// 新增场景（P2）
// ---------------------------------------------------------------------------

/// [命] 析构关闭 fd：RAII 核心契约，作用域结束后 fd 数恢复（fd 泄漏零直接断言）
TEST(SocketTest, DestructorClosesFd)
{
    const int before = FdCount();
    ASSERT_GT(before, 0);
    {
        Socket socket(Socket::IPV4, Socket::TCP);
        ASSERT_GT(socket.fd(), 0);
        EXPECT_EQ(FdCount(), before + 1);
    }
    EXPECT_EQ(FdCount(), before);
}

/// [界] INVALID_SOCKET 契约：isAvailable()==false，且析构触发 assert（Q12 现状锁定）
/// @attention 死亡测试：子进程内同时验证 isAvailable 为 false（否则以退出码 1 结束）
TEST(SocketTest, InvalidFdUnavailableAndDestructAborts)
{
    if (CODEBUS_TEST_UNDER_SANITIZER)
    {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            Socket socket(Socket::INVALID_SOCKET);
            if (socket.isAvailable())
            {
                ::_exit(1);
            }
            if (socket.fd() != Socket::INVALID_SOCKET)
            {
                ::_exit(1);
            }
            // 作用域结束：~Socket 的 assert(m_fd > 0) 触发 abort
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

/// [错] 二次 close 触发 assert（abort）——死亡测试（Q12 现状锁定）
TEST(SocketTest, DoubleCloseAborts)
{
    if (CODEBUS_TEST_UNDER_SANITIZER)
    {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            // 注意：宏参数内不可用带逗号的类型拼写与结构化绑定（逗号拆散宏参数）
            const auto fds = MakeSocketPair();
            if (fds.first < 0)
            {
                ::_exit(1);
            }
            ::close(fds.second);
            Socket socket(fds.first);
            if (!socket.close())
            {
                ::_exit(1);
            }
            socket.close(); // m_fd 已置 -1，二次 close 触发 assert
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

/// [错] 阻塞 connect 未监听端口返回 false（不抛异常，回环口立即 ECONNREFUSED）
TEST(SocketTest, ConnectRefusedReturnsFalse)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Socket client(Socket::IPV4, Socket::TCP);
    ASSERT_GT(client.fd(), 0);
    Address address("127.0.0.1", port);
    EXPECT_FALSE(client.connect(address));
}

/// [界] 非阻塞 listen socket 无连接时 accept 立即返回 fd<0（不挂起）
/// @attention accept() 对阻塞 listen socket 无连接时会永久阻塞，
///             故本用例服务端使用 NIO_TCP
TEST(SocketTest, AcceptWithoutConnectionReturnsInvalid)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Socket server(Socket::IPV4, Socket::NIO_TCP);
    ASSERT_GT(server.fd(), 0);
    Address address("127.0.0.1", port);
    ASSERT_TRUE(server.bind(address));
    ASSERT_TRUE(server.listen());

    auto [accepted_fd, peer] = server.accept();
    EXPECT_LT(accepted_fd, 0); // EAGAIN，无连接
}

/// [正] 已连接 socket 的本地/远端地址 IP 相等性（端口断言已由 TcpBindListenAcceptConnect 覆盖）
TEST(SocketTest, LocalRemoteAddrIpMatch)
{
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Socket server(Socket::IPV4, Socket::TCP);
    ASSERT_TRUE(server.bind(Address("127.0.0.1", port)));
    ASSERT_TRUE(server.listen());

    Socket client(Socket::IPV4, Socket::TCP);
    ASSERT_TRUE(client.connect(Address("127.0.0.1", port)));

    auto [accepted_fd, peer] = server.accept();
    ASSERT_GT(accepted_fd, 0);
    Socket accepted(accepted_fd);

    const auto local = client.getLocalAddr();
    const auto remote = client.getRemoteAddr();
    ASSERT_TRUE(local.has_value());
    ASSERT_TRUE(remote.has_value());
    EXPECT_EQ(local->ipString(), "127.0.0.1");
    EXPECT_EQ(remote->ipString(), "127.0.0.1");
}

/// [正] 选项关闭路径同样成功
TEST(SocketTest, OptionsOffStillSucceed)
{
    Socket socket(Socket::IPV4, Socket::TCP);
    ASSERT_GT(socket.fd(), 0);

    EXPECT_TRUE(socket.setTcpNoDelay(false));
    EXPECT_TRUE(socket.setKeepAlive(false));
    EXPECT_TRUE(socket.setReuseAddr(false));
    EXPECT_TRUE(socket.setReusePort(false));
}

/// [正] read(Buffer*) 将对端数据写入 Buffer 可写区（与 Buffer 单元衔接）
TEST(SocketTest, ReadIntoBuffer)
{
    auto [local_fd, peer_fd] = MakeSocketPair();
    ASSERT_GE(local_fd, 0);
    ASSERT_GE(peer_fd, 0);
    ScopedFd peer(peer_fd);
    Socket socket(local_fd);

    ASSERT_EQ(::write(peer.get(), "hello", 5), static_cast<ssize_t>(5));

    Buffer buffer(64);
    EXPECT_EQ(socket.read(&buffer), static_cast<ssize_t>(5));
    EXPECT_EQ(buffer.ReadableSize(), 5U);
    EXPECT_EQ(buffer.PeekAll().toString(), "hello");
}

/// [界] Family::UNIX + Type::TCP 创建成功（AF_UNIX 流式 socket 冒烟）
TEST(SocketTest, UnixFamilySocketCreated)
{
    Socket socket(Socket::UNIX, Socket::TCP);
    EXPECT_GT(socket.fd(), 0);
    EXPECT_TRUE(socket.isAvailable());
}

// ---------------------------------------------------------------------------
// 新增场景（UDP 轮 V0：数据报原语与选项）
// ---------------------------------------------------------------------------

/// [正] UDP 原语：loopback 双向 sendTo/recvFrom——数据与对端地址正确；
///      排空后 recvFrom 得 -1 且 errno==EAGAIN（MSG_DONTWAIT 单调用语义）
TEST(SocketTest, UdpLoopbackRoundTrip)
{
    Socket receiver(Socket::IPV4, Socket::NIO_UDP);
    Socket sender(Socket::IPV4, Socket::NIO_UDP);
    ASSERT_TRUE(receiver.isAvailable() && sender.isAvailable());

    // 注：Address 构造禁止 port 0（I2 契约），临时端口场景改用 GetFreePort
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    ASSERT_TRUE(receiver.bind(Address("127.0.0.1", port)));

    // 正向：sender → receiver
    ASSERT_TRUE(sender.sendTo("ping", 4, Address("127.0.0.1", port)));
    char buf[16]{};
    const auto [n1, peer1] = receiver.recvFrom(buf, sizeof(buf));
    ASSERT_EQ(n1, 4);
    EXPECT_EQ(std::string_view(buf, n1), "ping");
    EXPECT_EQ(peer1.ipString(), "127.0.0.1");

    // 回程：receiver → peer（用收到的对端地址）
    ASSERT_TRUE(receiver.sendTo("pong", 4, peer1));
    char buf2[16]{};
    const auto [n2, peer2] = sender.recvFrom(buf2, sizeof(buf2));
    ASSERT_EQ(n2, 4);
    EXPECT_EQ(std::string_view(buf2, n2), "pong");
    EXPECT_EQ(peer2.ipString(), "127.0.0.1");
    EXPECT_EQ(peer2.port(), port);

    // 排空后 EAGAIN（非阻塞语义）
    errno = 0;
    const auto [n3, dummy] = receiver.recvFrom(buf, sizeof(buf));
    (void)dummy;
    EXPECT_EQ(n3, -1);
    EXPECT_EQ(errno, EAGAIN);
}

/// [界] UDP 选项：setBroadcast/setMulticastTTL 置位成功（getsockopt 复核）；
///      joinGroup("224.0.0.254") 环境容错（无多播路由环境仅记录不失败）
TEST(SocketTest, UdpOptionsSettable)
{
    Socket socket(Socket::IPV4, Socket::NIO_UDP);
    ASSERT_TRUE(socket.isAvailable());

    EXPECT_TRUE(socket.setBroadcast(true));
    int broadcast = 0;
    socklen_t len = sizeof(broadcast);
    ASSERT_EQ(::getsockopt(socket.fd(), SOL_SOCKET, SO_BROADCAST, &broadcast, &len), 0);
    EXPECT_EQ(broadcast, 1);

    EXPECT_TRUE(socket.setMulticastTTL(2));
    int ttl = 0;
    len = sizeof(ttl);
    ASSERT_EQ(::getsockopt(socket.fd(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, &len), 0);
    EXPECT_EQ(ttl, 2);

    // ip_mreq 仅取组地址（端口无关），但 Address 构造要求合法端口
    const uint16_t any_port = GetFreePort();
    if (!socket.joinGroup(Address("224.0.0.254", any_port)))
    {
        GTEST_SKIP() << "环境无多播能力（joinGroup 失败按容错处理）";
    }
    EXPECT_TRUE(socket.leaveGroup(Address("224.0.0.254", any_port)));
}

} // namespace test
} // namespace bus
