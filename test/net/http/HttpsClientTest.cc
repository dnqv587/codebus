//***********************************************
// name      : HttpsClientTest.cc
// brief     : HttpClient https 端到端测试（H4-U5）：对 HttpsServer 回环——
//             verifyPeer 真实路径/SNI+IP 主机名校验/keep-alive 复用/TLS fatal
//             停重试/Reset 会话重建/verify 关闭容忍
// attention : 双 loop（server/client 各一）+ CallbackLatch（HttpClientTest 同款）；
//             证书位于 test/certs/（CODEBUS_TEST_CERT_DIR 定位）
//***********************************************
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/http/HttpClient.h"
#include "net/http/HttpServer.h"
#include "net/ssl/SslContext.h"

using namespace bus;
using bus::http::HttpClient;
using bus::http::HttpClientConfig;
using bus::http::HttpServer;
using bus::http::HttpServerConfig;
using bus::http::HttpRequest;
using bus::http::HttpResponse;
using bus::test::GetFreePort;
using bus::test::LoopThread;
using bus::test::RunInLoopSync;

namespace {

std::string CertDir() {
    return CODEBUS_TEST_CERT_DIR;
}

/// @brief 回调闭锁（HttpClientTest 同款——跨线程回调同步）
class CallbackLatch {
 public:
    void fire(std::optional<HttpResponse> r) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_result = std::move(r);
            m_fired = true;
        }
        m_cv.notify_one();
    }
    std::optional<HttpResponse> wait(int ms = 5000) {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (!m_cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return m_fired; })) {
            return std::nullopt;
        }
        return std::move(m_result);
    }
    bool fired() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_fired;
    }

 private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_fired = false;
    std::optional<HttpResponse> m_result;
};

/// @brief 等待谓词成立
template <typename Fn>
bool WaitUntil(Fn&& predicate, int timeoutMs = 5000) {
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

class HttpsClientTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_port = GetFreePort();
    }

    void TearDown() override {
        if (m_client) {
            std::atomic<bool> conn_down{false};
            m_client->setDisconnectedObserver([&] { conn_down = true; });
            m_client->Stop();
            // TLS fatal/已断场景连接已空——观察者不会再触发，两条件任一满足即过
            WaitUntil([&] { return conn_down.load() || m_client->connection() == nullptr; }, 10000);
            // FIFO 屏障：Stop 闭包及其回调链在 loop 内执行完毕后才析构（防 TSAN 竞争）
            RunInLoopSync(m_client_loop.loop(), [] {});
            m_client.reset();
        }
        if (m_server) {
            if (!m_stop_done.load()) {
                std::atomic<bool> done{false};
                m_server->Stop([&] { done = true; });
                EXPECT_TRUE(WaitUntil([&] { return done.load(); }, 10000));
            }
            m_server.reset();
        }
    }

    /// @brief TLS 服务端配置（默认受信证书；untrusted=true 换不受信证书）
    HttpServerConfig TlsServerCfg(bool untrusted = false) {
        HttpServerConfig cfg;
        cfg.tls.enabled = true;
        cfg.tls.certFile = CertDir() + (untrusted ? "/untrusted.crt" : "/server.crt");
        cfg.tls.keyFile = CertDir() + (untrusted ? "/untrusted.key" : "/server.key");
        return cfg;
    }

    /// @brief https 客户端配置（默认校验 + 信任测试 CA）
    HttpClientConfig TlsClientCfg(bool verify = true) {
        HttpClientConfig cfg;
        cfg.tls.enabled = true;
        cfg.tls.verifyPeer = verify;
        if (verify) {
            cfg.tls.caFile = CertDir() + "/ca.crt";
        }
        return cfg;
    }

    /// @brief 建服务端（不 Start——附加路由后再 StartServer；注册须在 freeze 前）
    void MakeServer(const HttpServer::Handler& get_handler) {
        m_server = std::make_unique<HttpServer>(m_server_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0,
                                                TlsServerCfg());
        m_server->Get("/x", get_handler);
    }

    void StartServer() {
        m_server->Start();
        RunInLoopSync(m_server_loop.loop(), [] {});  // 启动屏障
    }

    void MakeClient(HttpClientConfig cfg) {
        m_client = std::make_unique<HttpClient>(m_client_loop.loop(), "127.0.0.1:" + std::to_string(m_port), cfg);
    }

    uint16_t m_port = 0;
    LoopThread m_server_loop;
    LoopThread m_client_loop;
    std::unique_ptr<HttpServer> m_server;
    std::unique_ptr<HttpClient> m_client;
    std::atomic<bool> m_stop_done{false};
};

/// [正] GET over TLS：verifyPeer=true + 测试 CA → 200（主机名按 IP 命中 SAN IP:127.0.0.1）
TEST_F(HttpsClientTest, GetOverTlsVerified) {
    MakeServer([](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("hi https"));
    });
    StartServer();
    MakeClient(TlsClientCfg());
    m_client->Start();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Get("/x", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->getStatus(), http::HttpStatus::kOK);
    EXPECT_EQ(resp->getBody(), "hi https");
}

/// [正] POST over TLS：body 往返
TEST_F(HttpsClientTest, PostOverTls) {
    MakeServer([](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("done"));
    });
    m_server->Post("/p", [](const http::HttpRequest& req, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("len:").append(std::to_string(req.getBody().size())));
    });
    StartServer();
    MakeClient(TlsClientCfg());
    m_client->Start();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Post("/p", "application/octet-stream", std::string(4096, 'a'),
                   [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->getBody(), "len:4096");
}

/// [正] keep-alive：3 个串行请求同连接复用（client.connection() 同指针）
TEST_F(HttpsClientTest, KeepAliveReuseSameConnection) {
    int counter = 0;
    MakeServer([&counter](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("seq:").append(std::to_string(++counter)));
    });
    StartServer();
    MakeClient(TlsClientCfg());
    m_client->Start();

    void* conn_first = nullptr;
    for (int i = 1; i <= 3; ++i) {
        auto latch = std::make_shared<CallbackLatch>();
        m_client->Get("/x", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
        auto resp = latch->wait();
        ASSERT_TRUE(resp.has_value()) << "request " << i << " failed";
        EXPECT_EQ(resp->getBody(), "seq:" + std::to_string(i));
        TCPConnection::ptr c = m_client->connection();
        ASSERT_NE(c, nullptr);
        if (conn_first == nullptr) {
            conn_first = c.get();
        } else {
            EXPECT_EQ(c.get(), conn_first) << "connection not reused at request " << i;
        }
    }
}

/// [负] 不受信证书：verify=true → nullopt 恰一次；TLS fatal 停重试（无死循环——
///      观察窗内连接不重建）
TEST_F(HttpsClientTest, UntrustedCertFailsNoRetryLoop) {
    m_server = std::make_unique<HttpServer>(m_server_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0,
                                            TlsServerCfg(/*untrusted=*/true));
    m_server->Get("/x", [](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
    });
    StartServer();
    MakeClient(TlsClientCfg());  // 信任测试 CA——untrusted 证书必被拒
    m_client->Start();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Get("/x", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    EXPECT_FALSE(latch->wait().has_value()) << "untrusted cert must fail";
    EXPECT_TRUE(latch->fired()) << "callback must fire (failInflight), not just time out";

    // R6：TLS fatal 后 Connector 重试被停——观察窗内无新连接对象（旧实现此处会
    // 退避重连并反复握手失败）
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    EXPECT_TRUE(m_client->connection() == nullptr) << "retry loop not disabled after TLS fatal";
}

/// [正] Reset 弃用连接：下次 Request 重建（新 TLS 会话）
TEST_F(HttpsClientTest, ResetReconnectsNewTlsSession) {
    MakeServer([](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("r1"));
    });
    StartServer();
    MakeClient(TlsClientCfg());
    m_client->Start();

    auto l1 = std::make_shared<CallbackLatch>();
    m_client->Get("/x", [l1](std::optional<HttpResponse> r) { l1->fire(std::move(r)); });
    ASSERT_TRUE(l1->wait().has_value());
    TCPConnection::ptr first = m_client->connection();
    ASSERT_NE(first, nullptr);

    m_client->Reset();  // 弃用：下次 Request 重建
    auto l2 = std::make_shared<CallbackLatch>();
    m_client->Get("/x", [l2](std::optional<HttpResponse> r) { l2->fire(std::move(r)); });
    ASSERT_TRUE(l2->wait().has_value()) << "reconnect after Reset failed";
    TCPConnection::ptr second = m_client->connection();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first.get(), second.get());  // 新连接（新 TLS 会话）
}

/// [正] verifyPeer=false 容忍不受信证书（受控场景）
TEST_F(HttpsClientTest, VerifyDisabledToleratesUntrusted) {
    m_server = std::make_unique<HttpServer>(m_server_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0,
                                            TlsServerCfg(/*untrusted=*/true));
    m_server->Get("/x", [](const http::HttpRequest&, http::HttpResponse& resp) {
        resp.setStatus(http::HttpStatus::kOK);
        resp.setHeader("content-type", "text/plain");
        resp.setBody(std::string("tolerated"));
    });
    StartServer();
    MakeClient(TlsClientCfg(/*verify=*/false));
    m_client->Start();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Get("/x", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->getBody(), "tolerated");
}
