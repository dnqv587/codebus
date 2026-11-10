//***********************************************
// name      : HttpsServerTest.cc
// brief     : HttpsServer 端到端测试（H4-U4）：TlsConfig 聚合装配、TLS 上 HTTP
//             往返/keep-alive/sendfile 降级链/握手死线/坏证书/gzip over TLS
// attention : mini TLS 客户端为 U5 HttpClient https 落地前的过渡形态
//             （TcpClient + decorator attach + 原始帧 + HttpResponseParser）；
//             证书位于 test/certs/（CODEBUS_TEST_CERT_DIR 定位）
//***********************************************
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "Buffer.hpp"
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/EventLoop.h"
#include "net/http/HttpCompression.h"
#include "net/http/HttpMessage.h"
#include "net/http/HttpResponseParser.h"
#include "net/http/HttpServer.h"
#include "net/TcpClient.h"
#include "net/ssl/SslContext.h"
#include "net/ssl/SslSession.h"
#include "type.h"

using namespace bus;
using bus::http::HttpMethod;
using bus::test::GetFreePort;
using bus::test::LoopThread;
using bus::test::RunInLoopSync;
using bus::ssl::Role;
using bus::ssl::SslContext;
using bus::ssl::SslSession;

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

/// @brief 等待谓词成立
template <typename Fn>
bool WaitUntil(Fn&& predicate, int timeoutMs = 4000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

/// @brief mini TLS HTTP 客户端（过渡形态——U5 后可换 HttpClient 回归）
class MiniTlsClient {
 public:
    MiniTlsClient(EventLoop* loop, const std::string& addr, bool verify = true)
        : m_loop(loop), m_parser(64_MB), m_ctx(SslContext::createClient()) {
        if (verify) {
            m_ctx->useCaFile(CertDir() + "/ca.crt");
        } else {
            m_ctx->disableVerify();
        }
        m_client = std::make_unique<TcpClient>(loop, addr);
        m_client->setNewConnectionDecorator([this](const TCPConnection::ptr& conn) {
            conn->attachSsl(std::make_shared<SslSession>(Role::kClient, *m_ctx));
        });
        m_client->setConnectionCallback([this](const TCPConnection::ptr& c) {
            if (c->status() == TCPConnection::Disconnected) {
                m_down.store(true);
            }
        });
        m_client->setMessageCallback([this](const TCPConnection::ptr&, Buffer* buf) {
            Slice readable = buf->PeekAll();
            if (readable.size() == 0) {
                return;
            }
            // 喂入解析器（kComplete 自动复位可续喂——keep-alive 响应序列）
            const char* data = reinterpret_cast<const char*>(readable.data());
            size_t len = readable.size();
            http::ParseResult r = m_parser.Execute(data, len);
            if (r.event == http::ParseEvent::kComplete) {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_responses.push_back(m_parser.takeResponse());
            } else if (r.event == http::ParseEvent::kError) {
                m_parse_error.store(true);
            }
            buf->SkipRead(len);
        });
        m_client->connect();
    }

    /// @brief 发送原始 HTTP 请求（先关联方法——HEAD 响应跳 body 依赖）
    void request(HttpMethod method, std::string_view raw) {
        m_parser.start(method);
        if (auto c = connection()) {
            c->send(raw);
        }
    }

    TCPConnection::ptr connection() const {
        return m_client->connection();
    }
    bool down() const {
        return m_down.load();
    }
    bool parseError() const {
        return m_parse_error.load();
    }
    /// @brief 已收响应数（互斥——loop 线程产/测试线程读）
    size_t responseCount() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_responses.size();
    }
    /// @brief 取第 i 个响应的拷贝（跨线程安全）
    http::HttpResponse response(size_t i) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_responses.at(i);
    }

    /// @brief 收尾（TcpClient 析构契约：连接断开 + stop）
    void Shutdown() {
        RunInLoopSync(m_loop, [this]() {
            if (auto c = connection()) {
                c->close();
            }
        });
        WaitUntil([this] { return m_down.load(); });
        m_client->stop();
    }

 private:
    EventLoop* m_loop;
    std::unique_ptr<TcpClient> m_client;
    http::HttpResponseParser m_parser;
    ssl::SslContext::ptr m_ctx;
    mutable std::mutex m_mutex;
    std::vector<http::HttpResponse> m_responses;
    std::atomic<bool> m_down{false};
    std::atomic<bool> m_parse_error{false};
};

/// @brief 测试夹具：loop 线程 + 临时文件管理
class HttpsServerTest : public ::testing::Test {
 protected:
    void TearDown() override {
        if (!m_tmp_path.empty()) {
            ::unlink(m_tmp_path.c_str());
        }
        m_loop_thread.reset();
    }

    /// @brief 写临时载荷文件（sendfile 用例；失败返回空串——调用方须断言非空）
    std::string MakeTempFile(const std::string& content) {
        char path[] = "/tmp/codebus-https-test-XXXXXX";
        int fd = ::mkstemp(path);
        if (fd < 0) {
            return "";
        }
        size_t off = 0;
        while (off < content.size()) {
            ssize_t n = ::write(fd, content.data() + off, content.size() - off);
            if (n <= 0) {
                ::close(fd);
                return "";
            }
            off += static_cast<size_t>(n);
        }
        ::close(fd);
        m_tmp_path = path;
        return m_tmp_path;
    }

    /// @brief TLS 服务端配置（受信证书）
    http::HttpServerConfig TlsCfg() {
        http::HttpServerConfig cfg;
        cfg.tls.enabled = true;
        cfg.tls.certFile = CertDir() + "/server.crt";
        cfg.tls.keyFile = CertDir() + "/server.key";
        return cfg;
    }

    /// @brief 启动屏障（Start 异步投递——空任务 FIFO 串行）
    void StartBarrier() {
        RunInLoopSync(m_loop_thread->loop(), []() {});
    }

    /// @brief 服务端契约收尾（HttpServer 析构契约：Stop 且 DoneCallback 已触发）
    void StopServer(http::HttpServer& server) {
        std::atomic<bool> done{false};
        server.Stop([&done]() { done.store(true); });
        EXPECT_TRUE(WaitUntil([&] { return done.load(); }, 5000)) << "server Stop DoneCallback not fired";
    }

    std::unique_ptr<LoopThread> m_loop_thread;
    std::string m_tmp_path;
};

}  // namespace

/// [正] TLS 上 GET 往返：状态/头/body 全对
TEST_F(HttpsServerTest, GetRoundtripOverTls) {
    m_loop_thread = std::make_unique<LoopThread>();
    const uint16_t port = GetFreePort();
    http::HttpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0, TlsCfg());
    server.Get("/hello", [](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("hello tls"));
    });
    server.Start();
    StartBarrier();

    MiniTlsClient client(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(WaitUntil([&] { return client.connection() != nullptr && client.connection()->isSslEstablished(); }))
        << "TLS connection not established";
    client.request(HttpMethod::kGET, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");

    ASSERT_TRUE(WaitUntil([&] { return client.responseCount() == 1; })) << "no response over TLS";
    const auto resp = client.response(0);
    EXPECT_EQ(resp.getStatus(), http::HttpStatus::kOK);
    EXPECT_EQ(resp.getBody(), "hello tls");
    EXPECT_TRUE(resp.getHeader("date").has_value());
    EXPECT_TRUE(resp.getHeader("server").has_value());

    client.Shutdown();
    StopServer(server);
}

/// [正] TLS 上 POST 往返（body + Content-Length）
TEST_F(HttpsServerTest, PostRoundtripOverTls) {
    m_loop_thread = std::make_unique<LoopThread>();
    const uint16_t port = GetFreePort();
    http::HttpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0, TlsCfg());
    server.Post("/echo", [](const http::HttpRequest& req, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("got:").append(req.getBody()));
    });
    server.Start();
    StartBarrier();

    MiniTlsClient client(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(WaitUntil([&] { return client.connection() != nullptr && client.connection()->isSslEstablished(); }));
    const std::string body = MakePattern(32_KB);
    client.request(HttpMethod::kPOST, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);

    ASSERT_TRUE(WaitUntil([&] { return client.responseCount() == 1; }));
    const auto resp = client.response(0);
    EXPECT_EQ(resp.getStatus(), http::HttpStatus::kOK);
    EXPECT_EQ(resp.getBody(), "got:" + body);

    client.Shutdown();
    StopServer(server);
}

/// [正] keep-alive：同 TLS 连接 3 个请求，响应序正确、连接复用
TEST_F(HttpsServerTest, KeepAliveReusesSession) {
    m_loop_thread = std::make_unique<LoopThread>();
    const uint16_t port = GetFreePort();
    http::HttpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0, TlsCfg());
    server.Get("/seq", [](const http::HttpRequest& req, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("n=").append(req.getParam("n").value_or("?")));
    });
    server.Start();
    StartBarrier();

    MiniTlsClient client(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(WaitUntil([&] { return client.connection() != nullptr && client.connection()->isSslEstablished(); }));

    TCPConnection::ptr conn = client.connection();
    for (int i = 1; i <= 3; ++i) {
        client.request(HttpMethod::kGET, "GET /seq?n=" + std::to_string(i) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
        ASSERT_TRUE(WaitUntil([&, i] { return client.responseCount() == static_cast<size_t>(i); }))
            << "response " << i << " not arrived";
    }
    EXPECT_EQ(client.connection().get(), conn.get());  // 同一连接（未断线重连）

    ASSERT_EQ(client.responseCount(), 3u);
    for (int i = 1; i <= 3; ++i) {
        EXPECT_EQ(client.response(i - 1).getBody(), "n=" + std::to_string(i));
    }

    client.Shutdown();
    StopServer(server);
}

/// [正] sendfile over TLS：8MB 文件响应走 pumpPendingFileTls 降级链，字节全等
TEST_F(HttpsServerTest, SendfileOverTlsDowngradeChain) {
    m_loop_thread = std::make_unique<LoopThread>();
    const std::string payload = MakePattern(8_MB);
    const std::string path = MakeTempFile(payload);
    ASSERT_FALSE(path.empty()) << "temp file creation failed";

    const uint16_t port = GetFreePort();
    http::HttpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0, TlsCfg());
    server.Get("/file", [&path](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "application/octet-stream");
        ASSERT_TRUE(resp.setBodyFile(path));
    });
    server.Start();
    StartBarrier();

    MiniTlsClient client(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(WaitUntil([&] { return client.connection() != nullptr && client.connection()->isSslEstablished(); }));
    client.request(HttpMethod::kGET, "GET /file HTTP/1.1\r\nHost: localhost\r\n\r\n");

    ASSERT_TRUE(WaitUntil([&] { return client.responseCount() == 1; }, 10000)) << "file response incomplete";
    const auto resp = client.response(0);
    EXPECT_EQ(resp.getStatus(), http::HttpStatus::kOK);
    ASSERT_EQ(resp.getBody().size(), payload.size());
    EXPECT_EQ(std::memcmp(resp.getBody().data(), payload.data(), payload.size()), 0);

    client.Shutdown();
    StopServer(server);
}

/// [负] 握手死线：TCP 建立后不发 ClientHello（裸 TCP 连接）→ sweep 硬关
///      （keepAliveIdle/headerTimeout 调小使扫描周期 ≤1s，死线 1s 内生效）
TEST_F(HttpsServerTest, HandshakeTimeoutClosesSlowClient) {
    m_loop_thread = std::make_unique<LoopThread>();
    const uint16_t port = GetFreePort();
    auto cfg = TlsCfg();
    cfg.tls.handshakeTimeoutSec = 1;
    cfg.keepAliveIdleSec = 2;   // 缩短扫描周期（half = min(2,2)/2 = 1s）
    cfg.headerTimeoutSec = 2;
    http::HttpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0, cfg);
    server.Get("/x", [](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
    });
    server.Start();
    StartBarrier();

    // 裸 TCP（不 attach TLS）——服务端视角即「握手迟迟不开始」
    TcpClient raw(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port));
    std::atomic<bool> closed{false};
    raw.setConnectionCallback([&closed](const TCPConnection::ptr& c) {
        if (c->status() == TCPConnection::Disconnected) {
            closed.store(true);
        }
    });
    raw.connect();
    ASSERT_TRUE(WaitUntil([&] { return raw.connection() != nullptr; })) << "raw connect failed";
    EXPECT_TRUE(WaitUntil([&] { return closed.load(); }, 5000)) << "slow handshake not closed by sweep";

    raw.stop();  // 已断开：仅置停（析构契约）
    StopServer(server);
}

/// [负] 坏证书：服务端不受信证书 + 客户端校验 → 连接关闭、无响应
TEST_F(HttpsServerTest, BadCertificateRejected) {
    m_loop_thread = std::make_unique<LoopThread>();
    const uint16_t port = GetFreePort();
    auto cfg = TlsCfg();
    cfg.tls.certFile = CertDir() + "/untrusted.crt";
    cfg.tls.keyFile = CertDir() + "/untrusted.key";
    http::HttpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0, cfg);
    server.Get("/x", [](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setBody(std::string("never"));
    });
    server.Start();
    StartBarrier();

    MiniTlsClient client(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(WaitUntil([&] { return client.down(); }, 5000)) << "connection not closed on bad cert";
    EXPECT_EQ(client.responseCount(), 0u);  // 无任何 HTTP 响应到达

    client.Shutdown();  // 已断开：close 幂等 + stop 置停
    StopServer(server);
}

/// [正] gzip over TLS：内存 body 压缩往返（Content-Encoding + 解压校验）
TEST_F(HttpsServerTest, GzipOverTlsMemoryBody) {
    m_loop_thread = std::make_unique<LoopThread>();
    const std::string payload = MakePattern(64_KB);
    const uint16_t port = GetFreePort();
    auto cfg = TlsCfg();
    cfg.gzip.enabled = true;
    http::HttpServer server(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port), 0, cfg);
    server.Get("/big", [&payload](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(payload);
    });
    server.Start();
    StartBarrier();

    MiniTlsClient client(m_loop_thread->loop(), "127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(WaitUntil([&] { return client.connection() != nullptr && client.connection()->isSslEstablished(); }));
    client.request(HttpMethod::kGET,
                   "GET /big HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n");

    ASSERT_TRUE(WaitUntil([&] { return client.responseCount() == 1; }));
    const auto resp = client.response(0);
    EXPECT_EQ(resp.getStatus(), http::HttpStatus::kOK);
    auto ce = resp.getHeader("content-encoding");
    ASSERT_TRUE(ce.has_value());
    EXPECT_EQ(*ce, "gzip");
    std::string plain;
    ASSERT_EQ(http::GzipDecompress(resp.getBody(), plain, 8_MB), 0);
    EXPECT_EQ(plain, payload);

    client.Shutdown();
    StopServer(server);
}
