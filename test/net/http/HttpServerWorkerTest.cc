//***********************************************
// name      : HttpServerWorkerTest.cc
// brief     : HttpServer worker 业务池模式测试（单飞保序/回灌不假死/
//             阻塞 handler 隔离/Stop 排空/Date 头/错误路径）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "TestHelper.h"
#include "EventLoopTestHelper.h"
#include "net/http/HttpServer.h"

using namespace bus;
using namespace bus::http;
using namespace bus::test;

namespace
{

/// @brief 解析响应状态码
size_t ParseStatus(const std::string& raw)
{
    size_t sp = raw.find(' ');
    if (sp == std::string::npos) return 0;
    return static_cast<size_t>(atoi(raw.c_str() + sp + 1));
}

size_t ParseContentLength(const std::string& raw, size_t hdr_end)
{
    size_t pos = raw.find("Content-Length: ");
    if (pos == std::string::npos || pos > hdr_end) return 0;
    return static_cast<size_t>(atoll(raw.c_str() + pos + 16));
}

/// @brief 阻塞式 HTTP 测试客户端
class TestHttpClient
{
 public:
    ~TestHttpClient()
    {
        if (m_fd >= 0) ::close(m_fd);
    }

    bool connect(uint16_t port)
    {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool sendAll(std::string_view data)
    {
        size_t off = 0;
        while (off < data.size())
        {
            ssize_t n = ::send(m_fd, data.data() + off, data.size() - off, 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    std::string readResponse()
    {
        std::string buf;
        char tmp[8192];
        for (;;)
        {
            size_t hdr_end = buf.find("\r\n\r\n");
            if (hdr_end != std::string::npos)
            {
                size_t code = ParseStatus(buf);
                size_t cl = ParseContentLength(buf, hdr_end);
                size_t total = hdr_end + 4 + cl;
                if (buf.size() >= total)
                {
                    if (code >= 100 && code < 200)
                    {
                        buf.erase(0, total);
                        continue;
                    }
                    return buf.substr(0, total);
                }
            }
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<size_t>(n));
        }
        return buf;
    }

    std::string readAll()
    {
        std::string buf;
        char tmp[8192];
        for (;;)
        {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<size_t>(n));
        }
        return buf;
    }

 private:
    int m_fd = -1;
};

} // namespace

class HttpServerWorkerTest : public ::testing::Test
{
 protected:
    void SetUp() override
    {
        m_port = GetFreePort();
    }

    void TearDown() override
    {
        if (m_server)
        {
            if (!m_stop_done.load())
            {
                std::atomic<bool> done{false};
                m_server->Stop([&] { done = true; });
                ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
            }
            m_server.reset();
        }
    }

    /// @brief 建 worker 模式服务器
    void makeServer(uint32_t workers)
    {
        HttpServerConfig cfg{.workerNum = workers};
        m_server = std::make_unique<HttpServer>(
            m_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0, cfg);
    }

    void startAndWait()
    {
        m_server->Start();
        ASSERT_TRUE(WaitFor(
            [&]
            {
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) return false;
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
    std::atomic<bool> m_stop_done{false};
};

/// [正] 单飞保序：pipeline 两请求（前慢后快）响应仍按请求序
TEST_F(HttpServerWorkerTest, WorkerPreservesPipelineOrder)
{
    makeServer(2);
    m_server->Get("/slow",
                  [](const HttpRequest&, HttpResponse& r)
                  {
                      std::this_thread::sleep_for(std::chrono::milliseconds(50));
                      r.setBody(std::string("SLOW"));
                  });
    m_server->Get("/fast",
                  [](const HttpRequest&, HttpResponse& r)
                  {
                      std::this_thread::sleep_for(std::chrono::milliseconds(5));
                      r.setBody(std::string("FAST"));
                  });
    startAndWait();

    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    // 一次写出两个请求：/slow 在前 /fast 在后；若无单飞保序，FAST 会先回
    ASSERT_TRUE(c.sendAll(
        "GET /slow HTTP/1.1\r\nHost: t\r\n\r\nGET /fast HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string r1 = c.readResponse();    // 回灌路径处理第二请求
    std::string r2 = c.readResponse();
    ASSERT_NE(r1, "");
    ASSERT_NE(r2, "");
    EXPECT_NE(r1.find("SLOW"), std::string::npos);    // 响应按请求序
    EXPECT_NE(r2.find("FAST"), std::string::npos);
    size_t pos_slow = r1.find("SLOW");
    size_t pos_fast_in_r1 = r1.find("FAST");
    EXPECT_EQ(pos_fast_in_r1, std::string::npos);     // 第一响应内不混入第二响应
    (void)pos_slow;
}

/// [正] 阻塞 handler 不拖垮其他连接（worker 池隔离 IO 线程）
TEST_F(HttpServerWorkerTest, HandlerCanBlockWithoutStallingPeer)
{
    makeServer(2);
    m_server->Get("/block300",
                  [](const HttpRequest&, HttpResponse& r)
                  {
                      std::this_thread::sleep_for(std::chrono::milliseconds(300));
                      r.setBody(std::string("LATE"));
                  });
    m_server->Get("/instant",
                  [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("EARLY")); });
    startAndWait();

    TestHttpClient a;
    ASSERT_TRUE(a.connect(m_port));
    ASSERT_TRUE(a.sendAll("GET /block300 HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));    // 确保 A 已入队
    auto t0 = std::chrono::steady_clock::now();
    TestHttpClient b;
    ASSERT_TRUE(b.connect(m_port));
    ASSERT_TRUE(b.sendAll("GET /instant HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string rb = b.readResponse();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    EXPECT_NE(rb.find("EARLY"), std::string::npos);
    EXPECT_LT(elapsed.count(), 250);    // 未被 A 的 300ms 阻塞（IO 线程空闲）
    std::string ra = a.readResponse();
    EXPECT_NE(ra.find("LATE"), std::string::npos);
}

/// [正] Stop 排空：在途 worker 任务在 Stop 完成前执行（stopAndDrain 语义）
TEST_F(HttpServerWorkerTest, StopDrainsInflightWorkerTask)
{
    makeServer(1);
    std::atomic<bool> handler_done{false};
    m_server->Get("/slow",
                  [&](const HttpRequest&, HttpResponse& r)
                  {
                      std::this_thread::sleep_for(std::chrono::milliseconds(200));
                      handler_done = true;
                      r.setBody(std::string("drained"));
                  });
    startAndWait();

    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /slow HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));    // 任务已入队
    std::atomic<bool> stop_done{false};
    m_server->Stop([&] { stop_done = true; m_stop_done = true; });
    ASSERT_TRUE(WaitFor([&] { return stop_done.load(); }, std::chrono::milliseconds(10000)));
    EXPECT_TRUE(handler_done.load());    // 排空保证
}

/// [正] 响应含 RFC1123 GMT Date 头
TEST_F(HttpServerWorkerTest, DateHeaderPresent)
{
    makeServer(1);
    m_server->Get("/d", [](const HttpRequest&, HttpResponse& r) { r.setBody(std::string("ok")); });
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /d HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string resp = c.readResponse();
    EXPECT_NE(resp.find("Date: "), std::string::npos);
    EXPECT_NE(resp.find(" GMT\r\n"), std::string::npos);
}

/// [界] worker 模式下错误路径照常（解析在 IO 线程）
TEST_F(HttpServerWorkerTest, ErrorPathInWorkerMode)
{
    makeServer(2);
    startAndWait();
    TestHttpClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("NOT-HTTP GARBAGE\r\n\r\n"));
    std::string resp = c.readAll();
    EXPECT_EQ(ParseStatus(resp), 400u);
}
