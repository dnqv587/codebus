//***********************************************
// name      : HttpClientTest.cc
// brief     : 异步 HTTP 客户端测试（对自身 HttpServer 回环 + ResponseParser 罐头直喂）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <memory>
#include <optional>

#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/http/HttpClient.h"
#include "net/http/HttpServer.h"

using namespace bus;
using namespace bus::http;
using namespace bus::test;

namespace {

/// @brief 回调栅栏：等到回调发生并取走结果
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
            return std::nullopt;  // 超时：显式与"回调 nullopt"区分需看 fired
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

}  // namespace

class HttpClientTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_port = GetFreePort();
    }

    void TearDown() override {
        if (m_client) {
            std::atomic<bool> conn_down{false};
            m_client->setDisconnectedObserver([&] { conn_down = true; });
            m_client->Stop();
            WaitFor([&] { return conn_down.load(); }, std::chrono::milliseconds(10000));
            // FIFO 屏障：Stop 闭包及其回调链在 loop 内执行完毕后才析构（防 TSAN 竞争）
            RunInLoopSync(m_client_loop.loop(), [] {});
            m_client.reset();
        }
        if (m_server) {
            if (!m_stop_done.load()) {
                std::atomic<bool> done{false};
                m_server->Stop([&] { done = true; });
                ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
            }
            m_server.reset();
        }
    }

    void makeServer(HttpServerConfig cfg = {}) {
        m_server = std::make_unique<HttpServer>(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0, cfg);
    }

    void makeClient(HttpClientConfig cfg = {}) {
        m_client = std::make_unique<HttpClient>(m_client_loop.loop(),
                                                "127.0.0.1:" + std::to_string(m_port), cfg);
        m_client->setDisconnectedObserver([this] { m_disconnected = true; });
        m_client->Start();
    }

    LoopThread m_loop;         // server base loop
    LoopThread m_client_loop;  // client loop
    uint16_t m_port = 0;
    std::unique_ptr<HttpServer> m_server;
    std::unique_ptr<HttpClient> m_client;
    std::atomic<bool> m_disconnected{false};
    std::atomic<bool> m_stop_done{false};
};

/// [正] GET 回环
TEST_F(HttpClientTest, Get200RoundTrip) {
    makeServer();
    m_server->Get("/hi", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("hello")); });
    m_server->Start();
    makeClient();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Get("/hi", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->getStatus(), HttpStatus::kOK);
    EXPECT_EQ(resp->getBody(), "hello");
}

/// [正] POST 回显
TEST_F(HttpClientTest, PostEchoRoundTrip) {
    makeServer();
    m_server->Post("/e",
                   [](const HttpRequest& req, HttpResponse& r) { r.setBody(std::string(req.getBody())); });
    m_server->Start();
    makeClient();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Post("/e", "text/plain", "payload", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->getBody(), "payload");
}

/// [正] 404 是成功响应非错误
TEST_F(HttpClientTest, NotFoundIsSuccess) {
    makeServer();
    m_server->Start();
    makeClient();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Get("/nope", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->getStatus(), HttpStatus::kNOT_FOUND);
}

/// [正] keep-alive 复用：两请求同连接
TEST_F(HttpClientTest, KeepAliveReuse) {
    std::atomic<int> conns{0};
    makeServer();
    m_server->setConnectionObserver([&](const TCPConnection::ptr&, bool up) {
        if (up) {
            ++conns;
        }
    });
    m_server->Get("/a", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("A")); });
    m_server->Get("/b", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("B")); });
    m_server->Start();
    makeClient();

    auto l1 = std::make_shared<CallbackLatch>();
    m_client->Get("/a", [l1](std::optional<HttpResponse> r) { l1->fire(std::move(r)); });
    ASSERT_TRUE(l1->wait().has_value());
    auto l2 = std::make_shared<CallbackLatch>();
    m_client->Get("/b", [l2](std::optional<HttpResponse> r) { l2->fire(std::move(r)); });
    auto resp = l2->wait();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->getBody(), "B");
    EXPECT_LE(conns.load(), 1);  // 复用未重建
}

/// [界] 在途再 Request 被拒
TEST_F(HttpClientTest, RequestRejectedWhileInflight) {
    makeServer();
    m_server->Get("/slow", [](const HttpRequest&, HttpResponse& r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        r.setBody(std::string("s"));
    });
    m_server->Start();
    makeClient();

    auto l1 = std::make_shared<CallbackLatch>();
    m_client->Get("/slow", [l1](std::optional<HttpResponse> r) { l1->fire(std::move(r)); });
    auto l2 = std::make_shared<CallbackLatch>();
    m_client->Get("/other", [l2](std::optional<HttpResponse> r) { l2->fire(std::move(r)); });
    EXPECT_FALSE(l2->wait(300).has_value());  // 被拒（回调不触发；超时区分）
    EXPECT_FALSE(l2->fired());
    ASSERT_TRUE(l1->wait().has_value());  // 首请求正常
}

/// [正] Reset 半关语义：优雅关写方向，在途响应仍可送达
/// （硬断连路径的 nullopt 由 ResponseTimeoutFails 覆盖：failInflight+disconnect）
TEST_F(HttpClientTest, ResetHalfCloseAllowsInflightComplete) {
    makeServer();
    m_server->Get("/hang", [](const HttpRequest&, HttpResponse& r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        r.setBody(std::string("late"));
    });
    m_server->Start();
    makeClient();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Get("/hang", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    m_client->Reset();  // 优雅半关：请求已发出，响应仍可回
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());   // 在途完成（半关不杀读方向）
    EXPECT_EQ(resp->getBody(), "late");
}

/// [界] 响应超时：nullopt + 断连
TEST_F(HttpClientTest, ResponseTimeoutFails) {
    makeServer();
    m_server->Get("/slow", [](const HttpRequest&, HttpResponse& r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        r.setBody(std::string("late"));
    });
    m_server->Start();
    makeClient(HttpClientConfig{.responseTimeoutMs = 300});

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Get("/slow", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    auto resp = latch->wait(2000);
    EXPECT_FALSE(resp.has_value());
}

/// [正] 302 原样返回不跟随
TEST_F(HttpClientTest, RedirectNotFollowed) {
    makeServer();
    m_server->Get("/jump", [](const HttpRequest&, HttpResponse& r) { r.SendRedirect("/target"); });
    m_server->Start();
    makeClient();

    auto latch = std::make_shared<CallbackLatch>();
    m_client->Get("/jump", [latch](std::optional<HttpResponse> r) { latch->fire(std::move(r)); });
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->getStatus(), HttpStatus::kFOUND);
    ASSERT_TRUE(resp->getHeader("Location").has_value());
    EXPECT_EQ(*resp->getHeader("Location"), "/target");
}

/// [正] ResponseParser 罐头直喂：chunked 解码
TEST(HttpClientParserTest, ChunkedResponseDecoded) {
    HttpResponseParser parser;
    parser.start(HttpMethod::kGET);
    std::string raw =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n3\r\n wo\r\n0\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    ASSERT_EQ(r.event, ParseEvent::kComplete);
    HttpResponse resp = parser.takeResponse();
    EXPECT_EQ(resp.getStatus(), HttpStatus::kOK);
    EXPECT_EQ(resp.getBody(), "hello wo");
}

/// [正] HEAD 响应：关联方法后正确跳 body
TEST(HttpClientParserTest, HeadResponseNoBody) {
    HttpResponseParser parser;
    parser.start(HttpMethod::kHEAD);
    std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    ASSERT_EQ(r.event, ParseEvent::kComplete);
    HttpResponse resp = parser.takeResponse();
    EXPECT_EQ(resp.getBody(), "");
}

/// [界] 坏状态行 → 400 类错误
TEST(HttpClientParserTest, BadStatusLineRejected) {
    HttpResponseParser parser;
    parser.start(HttpMethod::kGET);
    std::string raw = "GARBAGE\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
}

/// [界] 超限 body 拒收
TEST(HttpClientParserTest, BodyTooLargeRejected) {
    HttpResponseParser parser(8);
    parser.start(HttpMethod::kGET);
    std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\naaaaaaaaa";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBodyTooLarge);
}

/// [界] 响应回调内调 Stop（H2 审查 P1-3：TimerManager 帧内自毁 UAF 的修复锁定——
///        RunInLoop 立即执行 vs QueueInLoop；回调在 IO 线程触发是文档契约场景）
TEST_F(HttpClientTest, StopInsideResponseCallbackIsSafe) {
    makeServer();
    m_server->Get("/x", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("ok")); });
    m_server->Start();
    makeClient();

    auto latch = std::make_shared<CallbackLatch>();
    HttpClient* raw = m_client.get();
    m_client->Get("/x", [latch, raw](std::optional<HttpResponse> r) {
        latch->fire(std::move(r));
        raw->Stop();  // 回调内停止（曾在 TimerManager 定时器帧内析构自身——UAF）
    });
    auto resp = latch->wait();
    ASSERT_TRUE(resp.has_value());  // 响应先达，Stop 后续清理安全排队
    std::atomic<bool> down{false};
    m_client->setDisconnectedObserver([&] { down = true; });
    // Stop 已排队：等断开观测 + 排空屏障（TearDown 同型）
    ASSERT_TRUE(WaitFor([&] { return down.load(); }, std::chrono::milliseconds(10000)));
    RunInLoopSync(m_client_loop.loop(), [] {});
    SUCCEED();  // 未崩溃即锁定（进程存活验证 UAF 修复）
}
