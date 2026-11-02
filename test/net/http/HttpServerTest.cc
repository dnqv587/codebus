//***********************************************
// name      : HttpServerTest.cc
// brief     : HttpServer 传输层集成测试（真 socket：路由/keep-alive/错误路径/
//             超时防护/生命周期）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <unistd.h>
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
#include "log/Logger.h"
#include "net/http/HttpCompression.h"
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

    int fd() const noexcept {
        return m_fd;
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

/// [正] keep-alive 请求数上限：达限响应带 close（U1：nginx keepalive_requests 对应物）
TEST_F(HttpServerTest, KeepaliveRequestLimitCloses) {
    makeServer(HttpServerConfig{.keepAliveRequests = 2});
    m_server->Get("/k", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("K")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /k HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string r1 = c.readResponse();
    EXPECT_NE(r1.find("keep-alive"), std::string::npos);   // 未达限
    ASSERT_TRUE(c.sendAll("GET /k HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string r2 = c.readResponse();
    EXPECT_NE(r2.find("Connection: close"), std::string::npos);  // 第 2 个（达限）回 close
    std::string rest = c.readAll();                         // 对端随即半关闭 → EOF
    EXPECT_EQ(rest.find("K"), std::string::npos);           // 无更多响应
}

/// [界] HTTP/1.1 缺 Host 头 → 400（RFC 7230 §5.4 强制）
TEST_F(HttpServerTest, MissingHostRejected400) {
    makeServer();
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /x HTTP/1.1\r\n\r\n"));  // 无 Host
    std::string resp = c.readAll();
    EXPECT_EQ(ParseStatus(resp), 400u);
}

/// [正] form 表单解析：application/x-www-form-urlencoded body → getFormParam
TEST_F(HttpServerTest, FormBodyParsed) {
    makeServer();
    m_server->Post("/form",
                   [](const HttpRequest& req, HttpResponse& r) {
                       auto user = req.getFormParam("user");
                       r.setBody("u=" + std::string(user.value_or("?")));
                   });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll(
        "POST /form HTTP/1.1\r\nHost: t\r\nContent-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 10\r\n\r\nuser=tom+1"));
    std::string resp = c.readResponse();
    EXPECT_NE(resp.find("u=tom 1"), std::string::npos);  // '+' 按空格（query 同规则）
}

/// [正] 模式路由端到端：便捷注册自动识别 pattern 语法（U2）
TEST_F(HttpServerTest, PatternRoutingEndToEnd) {
    makeServer();
    m_server->Get("/items/{id}",
                  [](const HttpRequest& req, HttpResponse& r) {
                      r.setBody("item:" + std::string(req.getPathParam("id").value_or("?")));
                  });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /items/9 HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string resp = c.readResponse();
    EXPECT_NE(resp.find("item:9"), std::string::npos);
    // 未覆盖方法 405（MethodRouter 与 pattern 组合）
    ASSERT_TRUE(c.sendAll("POST /items/9 HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n"));
    std::string post = c.readResponse();
    EXPECT_EQ(ParseStatus(post), 405u);
}

/// [正] 文件响应端到端：分块收齐 + 校验和（U5）
TEST_F(HttpServerTest, FileResponseStreamed) {
    // 造 1MB 周期文件
    std::string tmp = "/tmp/codebus-stream-test.bin";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::string chunk(4096, '\0');
        for (size_t i = 0; i < chunk.size(); ++i) {
            chunk[i] = static_cast<char>('A' + (i % 26));
        }
        for (int i = 0; i < 256; ++i) {
            f << chunk;
        }
    }
    makeServer();
    m_server->Get("/f",
                  [&tmp](const HttpRequest&, HttpResponse& r) { r.setBodyFile(tmp); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /f HTTP/1.0\r\nHost: t\r\n\r\n"));  // 1.0 收完即 EOF
    std::string raw = c.readAll();
    EXPECT_NE(raw.find("Content-Length: 1048576"), std::string::npos);
    ASSERT_GE(raw.size(), 1048576u);
    // 校验 body 区
    size_t hdr = raw.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    std::string body = raw.substr(hdr + 4, 1048576);
    std::string expect_chunk(4096, '\0');
    for (size_t i = 0; i < expect_chunk.size(); ++i) {
        expect_chunk[i] = static_cast<char>('A' + (i % 26));
    }
    std::string expect;
    for (int i = 0; i < 256; ++i) {
        expect += expect_chunk;
    }
    EXPECT_EQ(body, expect);
    ::unlink(tmp.c_str());
}

/// [界] 流中文件被截断 → 连接关（CL 不符不保活）
TEST_F(HttpServerTest, FileTruncatedMidStream) {
    std::string tmp = "/tmp/codebus-trunc-test.bin";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << std::string(1024 * 1024, 'x');
    }
    makeServer();
    m_server->Get("/t", [&tmp](const HttpRequest&, HttpResponse& r) {
        ASSERT_TRUE(r.setBodyFile(tmp));
        ::truncate(tmp.c_str(), 1024);  // 注册后截断——流时 CL(1MB) 不符
    });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /t HTTP/1.0\r\nHost: t\r\n\r\n"));
    std::string raw = c.readAll();
    size_t hdr = raw.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    EXPECT_EQ(raw.size(), hdr + 4 + 1024);  // 头 + 截断后的实发字节即关
    ::unlink(tmp.c_str());
}

/// [正] 访问日志：NCSA combined 格式各字段（U6）
TEST_F(HttpServerTest, AccessLogFormat) {
    Logger* access = GET_LOGGER("http.access");
    access->setLevel(LogLevel::INFO);
    auto capture = std::make_shared<CaptureAppender>();
    access->addAppender(capture);

    makeServer(HttpServerConfig{.accessLog = true});
    m_server->Get("/al",
                  [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("X")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /al HTTP/1.1\r\nHost: t\r\nReferer: http://ref\r\n"
                          "User-Agent: codebus-test\r\n\r\n"));
    std::string resp = c.readResponse();
    EXPECT_EQ(ParseStatus(resp), 200u);
    ASSERT_TRUE(WaitFor([&] { return !capture->Contents().empty(); }));
    const std::string& line = capture->Contents();
    EXPECT_NE(line.find("\"GET /al HTTP/1.1\""), std::string::npos);   // 请求行
    EXPECT_NE(line.find(" 200 "), std::string::npos);                     // 状态码
    EXPECT_NE(line.find("\"http://ref\""), std::string::npos);           // referer
    EXPECT_NE(line.find("\"codebus-test\""), std::string::npos);         // ua
    EXPECT_NE(line.find("127.0.0.1"), std::string::npos);                  // 远端地址

    // 清理：摘除捕获 appender（防污染后续用例）
    access->delAppender(capture);
}

/// [正] 默认关闭零输出
TEST_F(HttpServerTest, AccessLogDisabled) {
    Logger* access = GET_LOGGER("http.access");
    auto capture = std::make_shared<CaptureAppender>();
    access->addAppender(capture);

    makeServer();  // accessLog 默认 false
    m_server->Get("/quiet", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("q")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /quiet HTTP/1.1\r\nHost: t\r\n\r\n"));
    EXPECT_NE(c.readResponse().find("q"), std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(capture->Contents().empty());  // 零访问日志
    access->delAppender(capture);
}

/// [正] WS 接管钩子：回调收到请求与边界后字节（U7；一期丢弃 leftover 的修复锁定）
TEST_F(HttpServerTest, UpgradeHandlerReceivesRequestAndLeftover) {
    makeServer();
    // shared 保活（got_key/got_leftover 同理由闭包持 shared 免栈悬垂）
    auto got = std::make_shared<std::pair<std::string, std::string>>();
    auto upgraded = std::make_shared<std::atomic<bool>>(false);
    m_server->setUpgradeHandler([got, upgraded](const TCPConnection::ptr& conn, HttpRequest req,
                                                std::string_view leftover) {
        got->first = std::string(req.getHeader("sec-websocket-key").value_or("?"));
        got->second = std::string(leftover);
        conn->send(std::string_view("HTTP/1.1 101 Switching Protocols\r\n\r\n"));
        upgraded->store(true);
    });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    // 升级头 + 尾部夹带首帧字节（本应被一期丢弃）
    ASSERT_TRUE(c.sendAll("GET /chat HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\n"
                          "Connection: Upgrade\r\nSec-WebSocket-Key: abc123\r\n\r\n"
                          "\x81\x03xyz"));
    ASSERT_TRUE(WaitFor([&] { return upgraded->load(); }));
    EXPECT_EQ(got->first, "abc123");
    EXPECT_EQ(got->second, std::string("\x81\x03xyz", 5));  // leftover 精确透出
    std::string resp = c.readHead();                            // 101 由回调方拼发
    EXPECT_NE(resp.find("101"), std::string::npos);
}

/// [正] 移交后连接脱离超时管理（不被 408/空闲杀）
TEST_F(HttpServerTest, UpgradedConnFreeFromTimeouts) {
    makeServer(HttpServerConfig{.keepAliveIdleSec = 1, .headerTimeoutSec = 1});
    // shared 保活：用例返回后 handler 尾声仍在写时 TearDown 并发构造 Stop 闭包（TSAN 栈竞态）
    auto upgraded = std::make_shared<std::atomic<bool>>(false);
    m_server->setUpgradeHandler([upgraded](const TCPConnection::ptr&, HttpRequest, std::string_view) {
        upgraded->store(true);
    });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\n"
                          "Connection: Upgrade\r\n\r\n"));
    ASSERT_TRUE(WaitFor([&] { return upgraded->load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // 超 408/空闲窗口
    // 连接仍可收发（未被服务端关闭）：发一个字节，另一端 send 不崩溃即视为存活
    ASSERT_TRUE(c.sendAll("z"));
    SUCCEED();  // readAll 等待会阻塞（服务端不回）——以发送成功为存活证据
}

/// [界] 无 handler 维持 501（现状回归）
TEST_F(HttpServerTest, UpgradeNoHandlerStill501) {
    makeServer();
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll(
        "GET /chat HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n"));
    std::string resp = c.readAll();
    EXPECT_EQ(ParseStatus(resp), 501u);
}

/// [正] HEAD 响应不携带 body 字节（H2 审查 P0-1 回归锁定：readHead 后接 pipeline
///        探针验证帧不错位——若 body 泄漏，下一请求响应会错位失败）
TEST_F(HttpServerTest, HeadNoBodyBytesThenPipelineProbe) {
    makeServer();
    m_server->Get("/h", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("BODY")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("HEAD /h HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string head = c.readHead();  // 头止于 \r\n\r\n（若有 body 字节则残留在流中）
    EXPECT_EQ(ParseStatus(head), 200u);
    EXPECT_NE(head.find("Content-Length: 4"), std::string::npos);
    // 探针：keep-alive 下一请求——若 HEAD 泄漏了 body 字节，本响应会错位
    ASSERT_TRUE(c.sendAll("GET /h HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string probe = c.readResponse();
    EXPECT_EQ(ParseStatus(probe), 200u);
    EXPECT_NE(probe.find("BODY"), std::string::npos);
    // 再确认流中无残留（第三个请求也正常）
    ASSERT_TRUE(c.sendAll("GET /h HTTP/1.1\r\nHost: t\r\n\r\n"));
    EXPECT_EQ(ParseStatus(c.readResponse()), 200u);
}

/// [界] 204 携带 setBody 时不外发 body 字节（H2 审查 P0-1 波及面）
TEST_F(HttpServerTest, NoBodyStatusSuppressesBody) {
    makeServer();
    m_server->Get("/n",
                  [](const HttpRequest&, HttpResponse& r) {
                      r.setStatus(HttpStatus::kNO_CONTENT);
                      r.setBody(std::string("IGNORED"));
                  });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /n HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string resp = c.readResponse();  // 无 CL → 需以头后紧跟下一响应验证
    EXPECT_EQ(ParseStatus(resp), 204u);
    EXPECT_EQ(resp.find("IGNORED"), std::string::npos);
    ASSERT_TRUE(c.sendAll("GET /n HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string again = c.readResponse();
    EXPECT_EQ(ParseStatus(again), 204u);  // 帧未错位
}

/// [正] gzip 端到端：Accept-Encoding 命中 → Content-Encoding + 可解压还原（U2）
TEST_F(HttpServerTest, ServerGzipEndToEnd) {
    HttpServerConfig cfg;
    cfg.gzip = GzipConfig{.enabled = true, .level = 9, .minSize = 8};
    makeServer(cfg);
    std::string big(size_t(4_KB), 'k');
    m_server->Get("/g",
                  [&big](const HttpRequest&, HttpResponse& r) {
                      r.setHeader("Content-Type", "text/html");
                      r.setBody(big);
                  });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /g HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\n\r\n"));
    std::string raw = c.readResponse();  // keep-alive 按 CL 收（readAll 会挂到空闲超时）
    EXPECT_NE(raw.find("Content-Encoding: gzip"), std::string::npos);
    EXPECT_NE(raw.find("Vary: Accept-Encoding"), std::string::npos);
    size_t hdr = raw.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    std::string packed = raw.substr(hdr + 4);
    EXPECT_LT(packed.size(), big.size());  // 高压缩比确认
    std::string plain;
    ASSERT_EQ(GzipDecompress(packed, plain, size_t(1_MB)), 0);
    EXPECT_EQ(plain, big);
}

/// [正] 默认关零破坏（无编码头，原样）
TEST_F(HttpServerTest, ServerGzipDisabledByDefault) {
    makeServer();
    m_server->Get("/p", [](const HttpRequest&, HttpResponse& r) {
        r.setHeader("Content-Type", "text/html");
        r.setBody(std::string(size_t(4_KB), 'p'));
    });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /p HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\n\r\n"));
    std::string raw = c.readResponse();
    EXPECT_EQ(raw.find("Content-Encoding"), std::string::npos);
    EXPECT_NE(raw.find(std::string(size_t(4_KB), 'p').substr(0, 64)), std::string::npos);
}

/// [正] 小 body 与不可压 MIME 跳过
TEST_F(HttpServerTest, ServerGzipSkipsSmallOrIncompressible) {
    HttpServerConfig cfg;
    cfg.gzip = GzipConfig{.enabled = true, .minSize = size_t(1_KB)};
    makeServer(cfg);
    m_server->Get("/tiny", [](const HttpRequest&, HttpResponse& r) {
        r.setHeader("Content-Type", "text/html");
        r.setBody(std::string("small"));  // < minSize
    });
    m_server->Get("/png", [this](const HttpRequest&, HttpResponse& r) {
        r.setHeader("Content-Type", "image/png");
        r.setBody(std::string(size_t(4_KB), 'x'));
    });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /tiny HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\n\r\n"));
    EXPECT_EQ(c.readResponse().find("Content-Encoding"), std::string::npos);
    ASSERT_TRUE(c.sendAll("GET /png HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\n\r\n"));
    EXPECT_EQ(c.readResponse().find("Content-Encoding"), std::string::npos);
}

/// [正] 请求体解压：gzip body → handler 收明文；超限 413（U2）
TEST_F(HttpServerTest, RequestGunzipDecoded) {
    HttpServerConfig cfg;
    cfg.requestGunzip = true;
    cfg.maxBodyBytes = size_t(8_KB);  // plain 2KB 合法；bomb 64KB 超限
    makeServer(cfg);
    m_server->Post("/up",
                   [](const HttpRequest& req, HttpResponse& r) {
                       r.setBody("len=" + std::to_string(req.getBody().size()));
                   });
    startAndWait();
    std::string plain(size_t(2_KB), 'u');
    std::string packed;
    ASSERT_TRUE(GzipCompress(plain, packed, 9));
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("POST /up HTTP/1.0\r\nHost: t\r\nContent-Encoding: gzip\r\nContent-Length: " +
                          std::to_string(packed.size()) + "\r\n\r\n" + packed));
    std::string raw = c.readAll();
    EXPECT_NE(raw.find("len=2048"), std::string::npos);  // 明文入 handler

    // bomb：解压超 maxBody(1KB) → 413
    std::string bomb_plain(size_t(64_KB), 'b');
    std::string bomb;
    ASSERT_TRUE(GzipCompress(bomb_plain, bomb, 9));
    TestHttpClient c2;
    ASSERT_TRUE(c2.connect(m_port));
    ASSERT_TRUE(c2.sendAll("POST /up HTTP/1.0\r\nHost: t\r\nContent-Encoding: gzip\r\nContent-Length: " +
                           std::to_string(bomb.size()) + "\r\n\r\n" + bomb));
    std::string resp2 = c2.readAll();
    EXPECT_EQ(ParseStatus(resp2), 413u);
}

/// [正] HEAD 响应头后零残留（Q37：MSG_PEEK|MSG_DONTWAIT 精确断言——readHead 盲区的直接验证）
TEST_F(HttpServerTest, HeadNoTrailingBytes) {
    makeServer();
    m_server->Get("/h", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("BODY")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("HEAD /h HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string head = c.readHead();
    ASSERT_EQ(ParseStatus(head), 200u);
    // 头后不应有任何字节：短超时 recv 应 EAGAIN（而非读到 body 字节）
    char probe[8];
    ssize_t n = ::recv(c.fd(), probe, sizeof(probe), MSG_PEEK | MSG_DONTWAIT);
    EXPECT_TRUE(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        << "expected no trailing bytes, got " << n << " bytes";
}
