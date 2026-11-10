//***********************************************
// name      : TlsConnectionTest.cc
// brief     : TLS 注入 TCPConnection 端到端测试（H4-U3）：socketpair 双端 attach
//             SslSession + 单 EventLoop 驱动——握手/大块传输/握手期排队/半关/
//             RST/坏证书/装饰回调装配
// attention : 证书位于 test/certs/（CODEBUS_TEST_CERT_DIR 定位）；
//             明文等价性由既有 58 目标回归锁定（本目标只测 TLS 路径）
//***********************************************
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "Buffer.hpp"
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/EventLoop.h"
#include "net/TCPConnection.h"
#include "net/TcpClient.h"
#include "net/TcpServer.h"
#include "net/ssl/SslContext.h"
#include "net/ssl/SslSession.h"
#include "type.h"

using namespace bus;
using bus::test::GetFreePort;
using bus::test::LoopThread;
using bus::test::RunInLoopSync;
using bus::ssl::Role;
using bus::ssl::SslContext;
using bus::ssl::SslSession;
using bus::ssl::Want;

namespace {

std::string CertDir() {
    return CODEBUS_TEST_CERT_DIR;
}

/// @brief 确定性测试载荷
std::string MakePattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>((i * 31 + i / 251) & 0xFF);
    }
    return s;
}

/// @brief 收集器：msg_cb 拼接明文并消费缓冲
struct Collector {
    std::string data;
    void onMessage(const TCPConnection::ptr&, Buffer* buf) {
        Slice readable = buf->PeekAll();
        data.append(reinterpret_cast<const char*>(readable.data()), readable.size());
        buf->SkipRead(readable.size());
    }
};

/// @brief 等待谓词成立（loop 事件驱动推进，RunInLoopSync 往返即处理一批事件）
template <typename Fn>
bool WaitUntil(Fn&& predicate, int timeoutMs = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

}  // namespace

class TlsConnectionTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_loop_thread = std::make_unique<LoopThread>();
        // SOCK_NONBLOCK：生产路径的连接 fd 由 accept/Connector 保证非阻塞（EAGAIN 排队
        // 语义的前提）；socketpair 模拟须自设，否则大块传输时 loop 线程阻塞在 write
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, m_fds), 0) << strerror(errno);
    }

    void TearDown() override {
        // 优雅收尾：先 close（幂等——close() 在 loop 线程内联执行 handleClose 至终态），
        // 再单独一轮销毁（析构断言要求 Disconnected/Connecting）
        RunInLoopSync(m_loop_thread->loop(), [this]() {
            if (m_client) {
                m_client->close();
            }
            if (m_server) {
                m_server->close();
            }
        });
        RunInLoopSync(m_loop_thread->loop(), [this]() {
            m_client.reset();
            m_server.reset();
        });
        m_loop_thread.reset();
    }

    /// @brief 建立 attach 好的一对 TLS 连接（不 establish——由 ConnectPair 驱动）
    /// @param serverCert/serverKey 服务端证书（默认受信；untrusted* 模拟坏证书）
    /// @param clientVerify 客户端是否校验（true=信任测试 CA）
    void CreatePair(const char* serverCert = "server.crt", const char* serverKey = "server.key",
                    bool clientVerify = true) {
        m_server_ctx = SslContext::createServer(CertDir() + "/" + serverCert, CertDir() + "/" + serverKey);
        m_client_ctx = SslContext::createClient();
        if (clientVerify) {
            m_client_ctx->useCaFile(CertDir() + "/ca.crt");
        } else {
            m_client_ctx->disableVerify();
        }
        EventLoop* loop = m_loop_thread->loop();
        RunInLoopSync(loop, [this, loop]() {
            const Address local("127.0.0.1", 12345);
            const Address peer("127.0.0.1", 12346);
            m_server = std::make_shared<TCPConnection>(loop, "tls-server", m_fds[0], local, peer);
            m_server->attachSsl(std::make_shared<SslSession>(Role::kServer, *m_server_ctx));
            m_client = std::make_shared<TCPConnection>(loop, "tls-client", m_fds[1], peer, local);
            m_client->attachSsl(std::make_shared<SslSession>(Role::kClient, *m_client_ctx));
        });
    }

    /// @brief 双端 connectEstablished（触发握手）
    void ConnectPair() {
        RunInLoopSync(m_loop_thread->loop(), [this]() {
            m_server->connectEstablished();
            m_client->connectEstablished();
        });
    }

    /// @brief 等待双端 TLS 数据期
    bool WaitEstablished() {
        return WaitUntil([this]() {
            return RunInLoopSync(m_loop_thread->loop(), [this]() {
                return m_client->isSslEstablished() && m_server->isSslEstablished();
            });
        });
    }

    /// @brief 运行事件循环的线程
    std::unique_ptr<LoopThread> m_loop_thread;
    /// @brief socketpair
    int m_fds[2]{-1, -1};
    /// @brief TLS 服务端连接（持有 fds[0]）
    TCPConnection::ptr m_server;
    /// @brief TLS 客户端连接（持有 fds[1]）
    TCPConnection::ptr m_client;
    /// @brief 证书上下文（保活——会话经 OpenSSL 引用计数自立，保活是双保险）
    SslContext::ptr m_server_ctx;
    SslContext::ptr m_client_ctx;
};

/// [正] 握手完成 + 双向明文 echo：TLS 对上层 Buffer* 消费语义透明
TEST_F(TlsConnectionTest, HandshakeThenEchoBothWays) {
    CreatePair();
    Collector server_got, client_got;
    m_server->setMessageCallback(
        [&server_got](const TCPConnection::ptr& c, Buffer* buf) { server_got.onMessage(c, buf); });
    m_client->setMessageCallback(
        [&client_got](const TCPConnection::ptr& c, Buffer* buf) { client_got.onMessage(c, buf); });

    ConnectPair();
    ASSERT_TRUE(WaitEstablished()) << "TLS handshake did not complete";

    const std::string up = MakePattern(16_KB);
    const std::string down = MakePattern(8_KB);
    m_client->send(up);
    m_server->send(down);
    ASSERT_TRUE(WaitUntil([&] {
        return RunInLoopSync(m_loop_thread->loop(),
                             [&] { return server_got.data.size() == up.size() && client_got.data.size() == down.size(); });
    })) << "echo payloads not delivered (server=" << server_got.data.size() << "/" << up.size()
        << ", client=" << client_got.data.size() << "/" << down.size() << ")";
    EXPECT_EQ(std::memcmp(server_got.data.data(), up.data(), up.size()), 0);
    EXPECT_EQ(std::memcmp(client_got.data.data(), down.data(), down.size()), 0);
}

/// [正] 1MB 单向大块传输（跨多 TLS record；socketpair 缓冲有限必经输出缓冲排队路径）
TEST_F(TlsConnectionTest, LargeTransferMultiRecord) {
    CreatePair();
    Collector server_got;
    m_server->setMessageCallback(
        [&server_got](const TCPConnection::ptr& c, Buffer* buf) { server_got.onMessage(c, buf); });
    m_client->setMessageCallback([](const TCPConnection::ptr&, Buffer* buf) { buf->SkipRead(buf->ReadableSize()); });

    ConnectPair();
    ASSERT_TRUE(WaitEstablished());

    const std::string payload = MakePattern(1_MB);
    m_client->send(payload);  // 测试线程发送——queueSendData 拷贝闭包（D9 路径）
    ASSERT_TRUE(WaitUntil([&] {
        return RunInLoopSync(m_loop_thread->loop(), [&] { return server_got.data.size() == payload.size(); });
    })) << "got " << server_got.data.size() << "/" << payload.size();
    EXPECT_EQ(std::memcmp(server_got.data.data(), payload.data(), payload.size()), 0);
}

/// [正] 明文早于握手完成：establish 后立即 send（TLS 仍在握手）——会话内排队，
///      握手完成点自动 flush，对端全量收到
TEST_F(TlsConnectionTest, SendBeforeHandshakeCompletes) {
    CreatePair();
    Collector server_got;
    m_server->setMessageCallback(
        [&server_got](const TCPConnection::ptr& c, Buffer* buf) { server_got.onMessage(c, buf); });

    const std::string payload = MakePattern(64_KB);
    // 同一 loop 闭包内：establish（Connected + ClientHello 发出）后立即 send——
    // 此时 TLS 握手必然未完成（对端响应不可能在闭包内到达）
    RunInLoopSync(m_loop_thread->loop(), [this, &payload]() {
        m_client->connectEstablished();
        m_client->send(payload);
        m_server->connectEstablished();
    });

    ASSERT_TRUE(WaitEstablished());
    ASSERT_TRUE(WaitUntil(
        [&] { return RunInLoopSync(m_loop_thread->loop(), [&] { return server_got.data.size() == payload.size(); }); }))
        << "queued-before-handshake payload not fully delivered: " << server_got.data.size() << "/"
        << payload.size();
    EXPECT_EQ(std::memcmp(server_got.data.data(), payload.data(), payload.size()), 0);
}

/// [正] 优雅半关：shutdown() 发 close_notify → 对端 kClosedPeer → handleClose；
///      对端回执后双端 Disconnected
TEST_F(TlsConnectionTest, ShutdownSendsCloseNotify) {
    CreatePair();
    ConnectPair();
    ASSERT_TRUE(WaitEstablished());

    std::atomic<bool> server_closed{false}, client_closed{false};
    m_server->setConnectionCallback([&server_closed](const TCPConnection::ptr& c) {
        if (c->status() == TCPConnection::Disconnected) {
            server_closed.store(true);
        }
    });
    m_client->setConnectionCallback([&client_closed](const TCPConnection::ptr& c) {
        if (c->status() == TCPConnection::Disconnected) {
            client_closed.store(true);
        }
    });

    m_client->shutdown();  // 半关：close_notify 随密文发尽后关写
    EXPECT_TRUE(WaitUntil([&] { return server_closed.load() && client_closed.load(); }))
        << "close_notify graceful close incomplete (server=" << server_closed.load()
        << ", client=" << client_closed.load() << ")";
}

/// [负] 对端突拆：服务端优雅 close（close_notify 先行）后以 linger0 销毁 fd（RST 紧随）——
///      客户端无论先见 close_notify 还是 RST，均安全达终态（无崩溃/无悬挂）
TEST_F(TlsConnectionTest, PeerAbruptTeardownSafe) {
    CreatePair();
    ConnectPair();
    ASSERT_TRUE(WaitEstablished());

    std::atomic<bool> client_closed{false};
    m_client->setConnectionCallback([&client_closed](const TCPConnection::ptr& c) {
        if (c->status() == TCPConnection::Disconnected) {
            client_closed.store(true);
        }
    });

    RunInLoopSync(m_loop_thread->loop(), [this]() {
        struct linger lg { 1, 0 };
        ::setsockopt(m_server->fd(), SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        m_server->close();   // 优雅：close_notify 先达对端（客户端 kClosedPeer→handleClose）
        m_server.reset();    // fd 以 linger0 关闭：RST 紧随（客户端事件已摘，无害）
    });
    EXPECT_TRUE(WaitUntil([&] { return client_closed.load(); })) << "client not closed after peer teardown";
}

/// [负] 坏证书：服务端不受信证书 + 客户端校验 → 握手 fatal、双端安全收尾
TEST_F(TlsConnectionTest, BadCertificateFatalClose) {
    CreatePair("untrusted.crt", "untrusted.key", /*clientVerify=*/true);
    ConnectPair();

    std::atomic<bool> client_closed{false}, server_closed{false};
    m_client->setConnectionCallback([&client_closed](const TCPConnection::ptr& c) {
        if (c->status() == TCPConnection::Disconnected) {
            client_closed.store(true);
        }
    });
    m_server->setConnectionCallback([&server_closed](const TCPConnection::ptr& c) {
        if (c->status() == TCPConnection::Disconnected) {
            server_closed.store(true);
        }
    });

    EXPECT_FALSE(WaitEstablished()) << "handshake should NOT complete with untrusted cert";
    EXPECT_TRUE(WaitUntil([&] { return client_closed.load(); })) << "client not closed on TLS fatal";
    EXPECT_TRUE(WaitUntil([&] { return server_closed.load(); })) << "server not closed after alert";
}

/// [正] 装饰回调装配等价：TcpServer + TcpClient 双端 decorator 自动 attach——
///      H4 决策 D-2 的装配形态（HttpsServer/HttpClient 的 U4/U5 基础）
TEST(TlsConnectionDecoratorTest, ServerClientDecoratorAttachesTls) {
    LoopThread loop_thread;
    EventLoop* loop = loop_thread.loop();

    auto server_ctx = SslContext::createServer(CertDir() + "/server.crt", CertDir() + "/server.key");
    auto client_ctx = SslContext::createClient();
    client_ctx->useCaFile(CertDir() + "/ca.crt");

    const uint16_t port = GetFreePort();
    TcpServer server(loop, "127.0.0.1:" + std::to_string(port), 0);
    server.setNewConnectionDecorator([&server_ctx](const TCPConnection::ptr& conn) {
        conn->attachSsl(std::make_shared<SslSession>(Role::kServer, *server_ctx));
    });
    Collector server_got;
    server.setMessageCallback(
        [&server_got](const TCPConnection::ptr& c, Buffer* buf) { server_got.onMessage(c, buf); });
    server.Start();

    TcpClient client(loop, "127.0.0.1:" + std::to_string(port));
    client.setNewConnectionDecorator([&client_ctx](const TCPConnection::ptr& conn) {
        conn->attachSsl(std::make_shared<SslSession>(Role::kClient, *client_ctx));
    });
    // conn_cb 须在建连前注册（newConnection 把当时快照装配进连接——建连后再设不生效）
    std::atomic<bool> client_down{false};
    client.setConnectionCallback([&client_down](const TCPConnection::ptr& c) {
        if (c->status() == TCPConnection::Disconnected) {
            client_down.store(true);
        }
    });
    Collector client_got;
    client.setMessageCallback(
        [&client_got](const TCPConnection::ptr& c, Buffer* buf) { client_got.onMessage(c, buf); });
    client.connect();

    const std::string payload = MakePattern(32_KB);
    TCPConnection::ptr conn;
    EXPECT_TRUE(WaitUntil([&] {
        conn = client.connection();
        return conn != nullptr;
    })) << "connect did not establish";
    if (conn != nullptr) {
        conn->send(payload);  // 未就绪时 TcpClient 暂存，建连+握手后自动上路

        EXPECT_TRUE(WaitUntil([&] {
            return RunInLoopSync(loop, [&] {
                return server_got.data.size() == payload.size() && client.connection() != nullptr &&
                       client.connection()->isSslEstablished();
            });
        })) << "decorator-attached TLS roundtrip incomplete (server=" << server_got.data.size() << "/"
            << payload.size() << ")";
        if (server_got.data.size() == payload.size()) {
            EXPECT_EQ(std::memcmp(server_got.data.data(), payload.data(), payload.size()), 0);
        }
    }
    EXPECT_EQ(std::memcmp(server_got.data.data(), payload.data(), payload.size()), 0);

    // 收尾：显式关连接（stop() 只停 Connector 重试，不关已建连接）+ 客户端停连 +
    // 服务端全停（DoneCallback 契约——TcpServerTest 同款）
    std::atomic<bool> server_done{false};
    RunInLoopSync(loop, [&client]() {
        if (auto c = client.connection()) {
            c->close();
        }
    });
    EXPECT_TRUE(WaitUntil([&] { return client_down.load(); }));
    RunInLoopSync(loop, [&client]() { client.stop(); });
    RunInLoopSync(loop, [&server, &server_done]() { server.Stop([&server_done]() { server_done.store(true); }); });
    EXPECT_TRUE(WaitUntil([&] { return server_done.load(); }));
}
