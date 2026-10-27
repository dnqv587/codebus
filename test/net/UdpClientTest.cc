//***********************************************
// name      : UdpClientTest.cc
// brief     : net 模块单元测试：被测 src/net/UdpClient.h/.cc（同步模型：
//             无线程，调用线程直接收发；可选 bind、可选 connect 连接型）
// attention : receive 阻塞调用线程（超时参数 poll 语义）；UdpServer 充当回显对端
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "Buffer.hpp"
#include "log/Logger.h"
#include "net/Address.h"
#include "net/Socket.h"
#include "net/UdpClient.h"
#include "net/UdpServer.h"

#include <atomic>
#include <chrono>
#include <string>

namespace bus {
namespace test {

/// [正] 全组件闭环：UdpClient sendTo → UdpServer 回显 → receive 收到内容与对端
TEST(UdpClientTest, EchoRoundTrip) {
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);
    server.setMessageCallback([&server](std::shared_ptr<Buffer> data, const Address& peer) {
        server.sendTo(data->PeekAll().toString(), peer);  // 工作者线程内回源
    });
    server.Start();

    UdpClient client;  // 未绑定：首次 sendTo 内核分配临时端口
    ASSERT_TRUE(client.sendTo("ping", server.localAddr()));
    const auto dgram = client.receive(2000);
    ASSERT_TRUE(dgram.has_value());
    EXPECT_EQ(dgram->first->PeekAll().toString(), "ping");
    EXPECT_EQ(dgram->first->ReadableSize(), 4u);
    EXPECT_EQ(dgram->second.port(), port);

    server.Stop();
}

/// [正] 接收超时守约：有限超时按期返回 nullopt；0 为非阻塞单次（立即返回）
TEST(UdpClientTest, ReceiveTimeout) {
    UdpClient client;  // 未绑定、无流量：receive 必走超时路径
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(client.receive(150).has_value());
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_GE(elapsed, 140);                      // 超时守约（留调度裕度）
    EXPECT_FALSE(client.receive(0).has_value());  // 非阻塞单次：立即返回
}

/// [正] 未绑定客户端：sendTo 后内核分配临时端口——localAddr 与服务端观察一致
TEST(UdpClientTest, AutoBindOnSend) {
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);
    std::atomic<uint16_t> peer_port{0};
    server.setMessageCallback(
        [&](const std::shared_ptr<Buffer>&, const Address& peer) { peer_port.store(peer.port()); });
    server.Start();

    UdpClient client;
    ASSERT_TRUE(client.sendTo("hi", server.localAddr()));
    ASSERT_TRUE(WaitFor([&]() { return peer_port.load() != 0; }));

    const auto local = client.localAddr();
    ASSERT_TRUE(local.has_value());
    EXPECT_NE(local->port(), 0);                 // 临时端口已分配
    EXPECT_EQ(local->port(), peer_port.load());  // 与服务端观察到的对端一致

    server.Stop();
}

/// [正] 显式绑定：构造期 bind 指定端口，localAddr 即构造地址
TEST(UdpClientTest, ExplicitBind) {
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    UdpClient client("127.0.0.1:" + std::to_string(port));
    const auto local = client.localAddr();
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(local->port(), port);
    EXPECT_EQ(local->ipString(), "127.0.0.1");
}

/// [正] 连接型过滤：connect(A) 后他源（普通 UDP socket）报文被内核丢弃
TEST(UdpClientTest, ConnectFiltersForeignPeer) {
    const uint16_t port_a = GetFreePort();
    const uint16_t client_port = GetFreePort();
    ASSERT_GT(port_a, 0);
    ASSERT_GT(client_port, 0);
    UdpServer server_a("127.0.0.1:" + std::to_string(port_a), 1);
    server_a.setMessageCallback([](const std::shared_ptr<Buffer>&, const Address&) {});
    server_a.Start();

    UdpClient client("127.0.0.1:" + std::to_string(client_port));  // 显式端口：假冒源有处可发
    ASSERT_TRUE(client.connect(server_a.localAddr()));

    // 假冒源：非 A 的他源普通 UDP socket 向 client 端口发包——内核按 connect 过滤丢弃
    Socket impostor(Socket::IPV4, Socket::NIO_UDP);
    ASSERT_TRUE(impostor.isAvailable());
    const Address client_addr = Address::FromIpPortString("127.0.0.1:" + std::to_string(client_port));
    ASSERT_TRUE(impostor.sendTo("fake", 4, client_addr));
    EXPECT_FALSE(client.receive(300).has_value());  // 短超时：无他源投递（过滤锁定）

    server_a.Stop();
}

/// [正] 连接型发送：connect 后 send 走 write，对端回显经 receive 收到
TEST(UdpClientTest, ConnectedSendEcho) {
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);
    server.setMessageCallback([&server](std::shared_ptr<Buffer> data, const Address& peer) {
        server.sendTo(data->PeekAll().toString(), peer);
    });
    server.Start();

    UdpClient client;
    ASSERT_TRUE(client.connect(server.localAddr()));
    ASSERT_TRUE(client.send("ping"));  // 连接型发送
    const auto dgram = client.receive(2000);
    ASSERT_TRUE(dgram.has_value());
    EXPECT_EQ(dgram->first->PeekAll().toString(), "ping");
    EXPECT_EQ(dgram->second.port(), port);

    server.Stop();
}

/// [界] 连接型 ICMP 不可达：connect 未监听端口 → 错误在收路径暴露（nullopt +
///      日志）不挂死；重关联真实 server（同 socket）后收发恢复
TEST(UdpClientTest, IcmpUnreachableRecoverable) {
    const uint16_t dead_port = GetFreePort();  // 探测时空闲即无监听（与 UdpTest 同假设）
    ASSERT_GT(dead_port, 0);
    UdpClient client;
    ASSERT_TRUE(client.connect(Address::FromIpPortString("127.0.0.1:" + std::to_string(dead_port))));
    ASSERT_TRUE(client.send("knock"));
    // M-5：捕获 sys 日志断言错误暴露路径本身（POLLERR 消费分支
    // "receive error event" 或 recvFrom 直返错误分支 "receive error"）——
    // 区别于纯超时（无日志、整 2000ms 后 nullopt）
    Logger* sys_logger = GET_LOGGER("sys");
    auto capture = std::make_shared<CaptureAppender>();
    sys_logger->addAppender(capture);
    EXPECT_FALSE(client.receive(2000).has_value());
    const std::string captured = capture->Contents();
    sys_logger->delAppender(capture);
    EXPECT_TRUE(captured.find("receive error event") != std::string::npos ||
                captured.find("UdpClient receive error") != std::string::npos)
        << "ICMP 不可达未在收路径暴露（日志缺失）";

    // 重关联真实 server——客户端未损坏，收发恢复
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);
    server.setMessageCallback([&server](std::shared_ptr<Buffer> data, const Address& peer) {
        server.sendTo(data->PeekAll().toString(), peer);
    });
    server.Start();
    ASSERT_TRUE(client.connect(server.localAddr()));  // 重关联
    ASSERT_TRUE(client.send("again"));
    const auto dgram = client.receive(2000);
    ASSERT_TRUE(dgram.has_value());
    EXPECT_EQ(dgram->first->PeekAll().toString(), "again");

    server.Stop();
}

/// [界] 0 长数据报：回显收到空 Buffer（非空指针、ReadableSize==0）
TEST(UdpClientTest, EmptyDatagramRoundTrip) {
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);
    UdpServer server("127.0.0.1:" + std::to_string(port), 1);
    server.setMessageCallback([&server](std::shared_ptr<Buffer> data, const Address& peer) {
        server.sendTo(data->PeekAll().toString(), peer);  // 0 长也回显
    });
    server.Start();

    UdpClient client;
    ASSERT_TRUE(client.sendTo(std::string_view(""), server.localAddr()));
    const auto dgram = client.receive(2000);
    ASSERT_TRUE(dgram.has_value());
    EXPECT_NE(dgram->first, nullptr);
    EXPECT_EQ(dgram->first->ReadableSize(), 0u);

    server.Stop();
}

}  // namespace test
}  // namespace bus
