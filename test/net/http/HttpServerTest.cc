//***********************************************
// name      : HttpServerTest.cc
// brief     : HttpServer 传输层集成测试（真 socket：路由/keep-alive/错误路径/
//             超时防护/生命周期）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "TestHelper.h"
#include "EventLoopTestHelper.h"
#include "net/http/HttpServer.h"

using namespace bus;
using namespace bus::http;
using namespace bus::test;

namespace {

/// @brief 解析响应状态码（"HTTP/1.1 200 OK" → 200；无状态行返回 0）
size_t ParseStatus(const std::string& raw) {
    size_t sp = raw.find(' ');
    if (sp == std::string::npos)
        return 0;
    return static_cast<size_t>(atoi(raw.c_str() + sp + 1));
}

/// @brief 解析 Content-Length（框架输出精确形态）
size_t ParseContentLength(const std::string& raw, size_t hdr_end) {
    size_t pos = raw.find("Content-Length: ");
    if (pos == std::string::npos || pos > hdr_end)
        return 0;
    return static_cast<size_t>(atoll(raw.c_str() + pos + 16));
}

/// @brief 阻塞式 HTTP 测试客户端（raw socket）
class TestHttpClient {
 public:
    ~TestHttpClient() {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    bool connect(uint16_t port) {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0)
            return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool sendAll(std::string_view data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::send(m_fd, data.data() + off, data.size() - off, 0);
            if (n <= 0)
                return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    /// @brief 读到响应头结束（\r\n\r\n）即返回——HEAD 场景专用：按 RFC 响应可
    ///        携带"应有 Content-Length"但无 body，按 CL 等 body 会挂到空闲超时
    std::string readHead() {
        std::string buf;
        char tmp[8192];
        while (buf.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                break;
            }
            buf.append(tmp, static_cast<size_t>(n));
        }
        return buf;
    }

    /// @brief 读一个完整响应（按 Content-Length；跳过 1xx 中间响应）
    std::string readResponse() {
        std::string buf;
        char tmp[8192];
        for (;;) {
            size_t hdr_end = buf.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                size_t code = ParseStatus(buf);
                size_t cl = ParseContentLength(buf, hdr_end);
                size_t total = hdr_end + 4 + cl;
                if (buf.size() >= total) {
                    if (code >= 100 && code < 200) {
                        buf.erase(0, total);  // 跳过 100-continue
                        continue;
                    }
                    return buf.substr(0, total);
                }
            }
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0)
                break;
            buf.append(tmp, static_cast<size_t>(n));
        }
        return buf;
    }

    /// @brief 读到 EOF（错误响应+半关闭场景）
    std::string readAll() {
        std::string buf;
        char tmp[8192];
        for (;;) {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0)
                break;
            buf.append(tmp, static_cast<size_t>(n));
        }
        return buf;
    }

 private:
    int m_fd = -1;
};

}  // namespace

class HttpServerTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_port = GetFreePort();
    }

    void TearDown() override {
        if (m_server) {
            if (!m_stop_done.load())  // 用例内已 Stop 过（DoneCallback 已触发）则跳过等待
            {
                std::atomic<bool> done{false};
                m_server->Stop([&] { done = true; });
                ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
            }
            m_server.reset();  // 已停契约满足，正常析构（勿 release——泄漏）
        }
    }

    /// @brief 建服务器（单 loop 模式）
    void makeServer(HttpServerConfig cfg = {}) {
        m_server = std::make_unique<HttpServer>(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0, cfg);
    }

    /// @brief 启动并等待 acceptor 就绪（重试 connect）
    void startAndWait() {
        m_server->Start();
        ASSERT_TRUE(WaitFor(
            [&] {
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0)
                    return false;
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(m_port);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
                ::close(fd);
                return ok;
            },
            std::chrono::milliseconds(3000)));
    }

    LoopThread m_loop;
    uint16_t m_port = 0;
    std::unique_ptr<HttpServer> m_server;
    /// @brief 用例内已触发 Stop DoneCallback（TearDown 跳过等待）
    std::atomic<bool> m_stop_done{false};
};

/// [正] GET 200 与回显
TEST_F(HttpServerTest, Get200) {
    makeServer();
    m_server->Get("/hi", [](const HttpRequest& req, HttpResponse& resp) { resp.setBody(std::string("hello")); });
    startAndWait();

    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /hi HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string resp = c.readResponse();
    EXPECT_EQ(ParseStatus(resp), 200u);
    EXPECT_NE(resp.find("hello"), std::string::npos);
    EXPECT_NE(resp.find("Content-Length: 5"), std::string::npos);
}

/// [正] 未注册路径 404
TEST_F(HttpServerTest, NotFound404) {
    makeServer();
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /nope HTTP/1.1\r\nHost: t\r\n\r\n"));
    EXPECT_EQ(ParseStatus(c.readResponse()), 404u);
}

/// [正] keep-alive：同连接顺序两请求
TEST_F(HttpServerTest, KeepAliveTwoRequests) {
    makeServer();
    m_server->Get("/a", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("A")); });
    m_server->Get("/b", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("B")); });
    startAndWait();

    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /a HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string r1 = c.readResponse();
    ASSERT_TRUE(c.sendAll("GET /b HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string r2 = c.readResponse();
    EXPECT_NE(r1.find("A"), std::string::npos);
    EXPECT_NE(r2.find("B"), std::string::npos);
    EXPECT_NE(r2.find("keep-alive"), std::string::npos);
}

/// [正] Connection: close → 响应回 close 且对端 EOF
TEST_F(HttpServerTest, ConnectionCloseCloses) {
    makeServer();
    m_server->Get("/x", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("X")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /x HTTP/1.0\r\nHost: t\r\n\r\n"));  // 1.0 默认 close
    std::string resp = c.readAll();
    EXPECT_NE(resp.find("Connection: close"), std::string::npos);
    EXPECT_NE(resp.find("X"), std::string::npos);
}

/// [正] pipeline：两请求一次写出，响应按序
TEST_F(HttpServerTest, PipelineOrder) {
    makeServer();
    m_server->Get("/p1", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("R1")); });
    m_server->Get("/p2", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("R2")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /p1 HTTP/1.1\r\nHost: t\r\n\r\nGET /p2 HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string r1 = c.readResponse();
    std::string r2 = c.readResponse();
    EXPECT_NE(r1.find("R1"), std::string::npos);
    EXPECT_NE(r2.find("R2"), std::string::npos);
    EXPECT_LT(r1.find("R1"), r1.find("R2") == std::string::npos ? r1.size() : r1.find("R2"));
}

/// [正] HEAD：CL 正确无 body
TEST_F(HttpServerTest, HeadNoBody) {
    makeServer();
    m_server->Get("/h", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("BODY")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("HEAD /h HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string resp = c.readHead();  // HEAD 响应有应有 CL 但无 body，按头终止读
    EXPECT_EQ(ParseStatus(resp), 200u);
    EXPECT_NE(resp.find("Content-Length: 4"), std::string::npos);
    EXPECT_EQ(resp.find("BODY"), std::string::npos);
}

/// [正] POST body 回显
TEST_F(HttpServerTest, PostBodyEcho) {
    makeServer();
    m_server->Post("/echo", [](const HttpRequest& req, HttpResponse& r) { r.setBody(std::string(req.getBody())); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 6\r\n\r\npiping"));
    std::string resp = c.readResponse();
    EXPECT_NE(resp.find("piping"), std::string::npos);
}

/// [正] query 参数与 Cookie 解析
TEST_F(HttpServerTest, QueryAndCookies) {
    makeServer();
    m_server->Get("/greet", [](const HttpRequest& req, HttpResponse& r) {
        auto name = req.getParam("name");
        auto sid = req.getCookie("sid");
        r.setBody(std::string(*name) + ":" + std::string(sid.value_or("-")));
    });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /greet?name=tom HTTP/1.1\r\nHost: t\r\nCookie: sid=42\r\n\r\n"));
    std::string resp = c.readResponse();
    EXPECT_NE(resp.find("tom:42"), std::string::npos);
}

/// [正] Expect: 100-continue → 先 100 再最终响应
TEST_F(HttpServerTest, Expect100Continue) {
    makeServer();
    m_server->Post("/big", [](const HttpRequest& req, HttpResponse& r) {
        r.setBody(std::string("got ") + std::to_string(req.getContentLength()));
    });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("POST /big HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\nabc"));
    std::string resp = c.readResponse();  // readResponse 内部跳过 1xx
    EXPECT_NE(resp.find("got 3"), std::string::npos);
}

/// [界] 垃圾字节 → 400 且连接关
TEST_F(HttpServerTest, BadRequest400) {
    makeServer();
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("NOT-HTTP GARBAGE\r\n\r\n"));
    std::string resp = c.readAll();
    EXPECT_EQ(ParseStatus(resp), 400u);
}

/// [界] CL 超限 → 413 且连接关（cfg.maxBodyBytes=16）
TEST_F(HttpServerTest, BodyTooLarge413) {
    makeServer(HttpServerConfig{.maxBodyBytes = 16});
    m_server->Post("/s", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("s")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    std::string big(64, 'x');
    ASSERT_TRUE(c.sendAll("POST /s HTTP/1.1\r\nHost: t\r\nContent-Length: 64\r\n\r\n" + big));
    std::string resp = c.readAll();
    EXPECT_EQ(ParseStatus(resp), 413u);
}

/// [界] 90KB 头 → 431
TEST_F(HttpServerTest, HeaderTooLarge431) {
    makeServer();
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    std::string raw = "GET / HTTP/1.1\r\nX-Big: " + std::string(90 * 1024, 'a') + "\r\n\r\n";
    ASSERT_TRUE(c.sendAll(raw));
    std::string resp = c.readAll();
    EXPECT_EQ(ParseStatus(resp), 431u);
}

/// [界] 慢速头 → 408（headerTimeoutSec=1，扫描周期 1s）
TEST_F(HttpServerTest, SlowHeader408) {
    makeServer(HttpServerConfig{.headerTimeoutSec = 1});
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET / HT"));  // 半截头后停
    std::string resp = c.readAll();
    EXPECT_EQ(ParseStatus(resp), 408u);
}

/// [界] 空闲连接超时关闭（keepAliveIdleSec=1）
TEST_F(HttpServerTest, IdleTimeoutCloses) {
    makeServer(HttpServerConfig{.keepAliveIdleSec = 1});
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));  // 不发任何数据
    std::string resp = c.readAll();  // 应收到 EOF（可能伴随无响应直接关）
    (void)resp;
    SUCCEED();  // readAll 返回即对端已关
}

/// [正] Start 幂等 / Stop DoneCallback 触发一次
TEST_F(HttpServerTest, StartStopIdempotent) {
    makeServer();
    m_server->Start();
    m_server->Start();  // 幂等忽略
    startAndWait();
    std::atomic<int> done_count{0};
    m_server->Stop([&] {
        ++done_count;
        m_stop_done = true;
    });
    m_server->Stop([&] { ++done_count; });  // 重复 Stop 忽略（TcpServer 语义）
    ASSERT_TRUE(WaitFor([&] { return done_count.load() > 0; }));
    EXPECT_EQ(done_count.load(), 1);
}

/// [正] handler 内 Stop 安全（D39 queue-only 同型）
TEST_F(HttpServerTest, StopFromHandlerIsSafe) {
    makeServer();
    HttpServer* srv = m_server.get();
    m_server->Get("/stop", [srv, this](const HttpRequest&, HttpResponse& r) {
        r.setBody(std::string("bye"));
        srv->Stop([this] { m_stop_done = true; });  // queue-only 安全
    });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /stop HTTP/1.1\r\nHost: t\r\n\r\n"));
    (void)c.readAll();  // 响应或关闭均可接受（Stop 与 flush 竞态不设断言）
    ASSERT_TRUE(WaitFor([&] { return m_stop_done.load(); }, std::chrono::milliseconds(10000)));
}

/// [正] 32 并发连接各一请求
TEST_F(HttpServerTest, Concurrency32) {
    makeServer();
    std::atomic<int> hits{0};
    m_server->Get("/c", [&hits](const HttpRequest&, HttpResponse& r) { r.setBody("hit " + std::to_string(++hits)); });
    startAndWait();

    std::vector<std::unique_ptr<TestHttpClient>> clients;
    for (int i = 0; i < 32; ++i) {
        auto c = std::make_unique<TestHttpClient>();
        ASSERT_TRUE(c->connect(m_port));
        ASSERT_TRUE(c->sendAll("GET /c HTTP/1.1\r\nHost: t\r\n\r\n"));
        clients.push_back(std::move(c));
    }
    int ok = 0;
    for (auto& c : clients) {
        std::string resp = c->readResponse();
        if (ParseStatus(resp) == 200)
            ++ok;
    }
    EXPECT_EQ(ok, 32);
    EXPECT_EQ(hits.load(), 32);
}

/// [正] 慢 body 不被 408 斩杀（P1 修复锁定：408 仅覆盖头阶段）
TEST_F(HttpServerTest, SlowBodyNotKilledBy408) {
    makeServer(HttpServerConfig{.headerTimeoutSec = 1});
    m_server->Post("/slow", [](const HttpRequest& req, HttpResponse& r) { r.setBody(std::string(req.getBody())); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("POST /slow HTTP/1.1\r\nHost: t\r\nContent-Length: 6\r\n\r\nabc"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // 头已完整，body 慢到
    ASSERT_TRUE(c.sendAll("def"));
    std::string resp = c.readResponse();  // 不应收到 408
    EXPECT_EQ(ParseStatus(resp), 200u);
    EXPECT_NE(resp.find("abcdef"), std::string::npos);
}

/// [正] 便捷注册方法路由：同路径多方法各归其位、未注册方法 405+Allow（P1 修复锁定）
TEST_F(HttpServerTest, MethodRouterConflict) {
    makeServer();
    m_server->Get("/m", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("GET-H")); });
    m_server->Post("/m", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("POST-H")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /m HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string g = c.readResponse();
    EXPECT_EQ(ParseStatus(g), 200u);
    EXPECT_NE(g.find("GET-H"), std::string::npos);
    ASSERT_TRUE(c.sendAll("POST /m HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n"));
    std::string p = c.readResponse();
    EXPECT_EQ(ParseStatus(p), 200u);
    EXPECT_NE(p.find("POST-H"), std::string::npos);
    ASSERT_TRUE(c.sendAll("PUT /m HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n"));
    std::string put = c.readResponse();
    EXPECT_EQ(ParseStatus(put), 405u);  // 未注册方法
    EXPECT_NE(put.find("Allow: GET, POST"), std::string::npos);
    ASSERT_TRUE(c.sendAll("HEAD /m HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string h = c.readHead();  // HEAD 回落 GET
    EXPECT_EQ(ParseStatus(h), 200u);
}

/// [正] 服务端层 Upgrade → 501（parser 层用例的服务端闭环）
TEST_F(HttpServerTest, ServerUpgrade501) {
    makeServer();
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /chat HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n"));
    std::string resp = c.readAll();
    EXPECT_EQ(ParseStatus(resp), 501u);
}
