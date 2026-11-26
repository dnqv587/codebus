//***********************************************
// name      : HttpConnectionPoolTest.cc
// brief     : 单 host 连接池测试（对自身 HttpServer 回环；并发调度/排队/Stop 收口）
// attention : 池与实例共用客户端 loop；TearDown 经 Stop done cb + FIFO 屏障后析构
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/http/HttpConnectionPool.h"
#include "net/http/HttpServer.h"

using namespace bus;
using namespace bus::http;
using namespace bus::test;

namespace {

/// @brief 回调栅栏（HttpClientTest 同款）
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

 private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_fired = false;
    std::optional<HttpResponse> m_result;
};

}  // namespace

class HttpConnectionPoolTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_port = GetFreePort();
    }
    void TearDown() override {
        if (m_pool) {
            std::atomic<bool> done{false};
            m_pool->Stop([&] { done = true; });
            ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
            RunInLoopSync(m_client_loop.loop(), [] {});
            m_pool.reset();
        }
        if (m_server) {
            std::atomic<bool> done{false};
            m_server->Stop([&] { done = true; });
            ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
            m_server.reset();
        }
    }
    void makeServer(HttpServerConfig cfg = {}) {
        m_server = std::make_unique<HttpServer>(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0, cfg);
    }
    void makePool(HttpPoolConfig cfg = {}) {
        m_pool = std::make_unique<HttpConnectionPool>(m_client_loop.loop(),
                                                      "127.0.0.1:" + std::to_string(m_port), cfg);
        m_pool->Start();
    }

    LoopThread m_loop;
    LoopThread m_client_loop;
    uint16_t m_port = 0;
    std::unique_ptr<HttpServer> m_server;
    std::unique_ptr<HttpConnectionPool> m_pool;
};

/// [正] 并发请求经多连接并行（H5.2 P1）：N=2 + 慢 handler，两请求须走两条连接
TEST_F(HttpConnectionPoolTest, ConcurrentRequestsUseMultipleConnections) {
    makeServer(HttpServerConfig{.workerNum = 2});
    std::mutex mu;
    std::set<std::string> peers;
    m_server->Get("/slow", [&](const HttpRequest& req, HttpResponse& r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            std::lock_guard<std::mutex> lk(mu);
            peers.insert(std::string(req.getRemoteAddr()));
        }
        r.setBody(std::string("ok"));
    });
    m_server->Start();
    makePool(HttpPoolConfig{.maxConnections = 2});

    auto l1 = std::make_shared<CallbackLatch>();
    auto l2 = std::make_shared<CallbackLatch>();
    m_pool->Get("/slow", [l1](std::optional<HttpResponse> r) { l1->fire(std::move(r)); });
    m_pool->Get("/slow", [l2](std::optional<HttpResponse> r) { l2->fire(std::move(r)); });
    auto r1 = l1->wait();
    auto r2 = l2->wait();
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->getStatus(), HttpStatus::kOK);
    EXPECT_EQ(r2->getStatus(), HttpStatus::kOK);
    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(peers.size(), 2u) << "两并发请求应经两条连接（单连接会串行化）";
}
/// [正] 全忙 FIFO 排队（H5.2 P2）：N=1 三请求串行保序，单连接
TEST_F(HttpConnectionPoolTest, FifoQueueWhenAllBusy) {
    makeServer();
    std::mutex mu;
    std::vector<std::string> order;
    m_server->Get("/seq", [&](const HttpRequest& req, HttpResponse& r) {
        std::lock_guard<std::mutex> lk(mu);
        order.push_back(std::string(req.getQuery()));
        r.setBody(std::string("ok"));
    });
    m_server->Start();
    makePool(HttpPoolConfig{.maxConnections = 1});

    std::vector<std::shared_ptr<CallbackLatch>> latches;
    for (int i = 0; i < 3; ++i) {
        auto l = std::make_shared<CallbackLatch>();
        latches.push_back(l);
        m_pool->Get("/seq?i=" + std::to_string(i), [l](std::optional<HttpResponse> r) { l->fire(std::move(r)); });
    }
    for (auto& l : latches) {
        auto r = l->wait();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->getStatus(), HttpStatus::kOK);
    }
    std::lock_guard<std::mutex> lk(mu);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "i=0");
    EXPECT_EQ(order[1], "i=1");
    EXPECT_EQ(order[2], "i=2");  // FIFO 保序
}

/// [正] 惰性创建（H5.2 P3）：N=3 单请求只建一条连接
TEST_F(HttpConnectionPoolTest, LazyCreationUpToCap) {
    makeServer();
    std::mutex mu;
    std::set<std::string> peers;
    m_server->Get("/one", [&](const HttpRequest& req, HttpResponse& r) {
        std::lock_guard<std::mutex> lk(mu);
        peers.insert(std::string(req.getRemoteAddr()));
        r.setBody(std::string("ok"));
    });
    m_server->Start();
    makePool(HttpPoolConfig{.maxConnections = 3});

    auto l = std::make_shared<CallbackLatch>();
    m_pool->Get("/one", [l](std::optional<HttpResponse> r) { l->fire(std::move(r)); });
    auto r = l->wait();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->getStatus(), HttpStatus::kOK);
    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(peers.size(), 1u);
}

/// [正] Stop 收口（H5.2 P5）：在途+排队全部 nullopt exactly-once，done cb 触发
TEST_F(HttpConnectionPoolTest, StopFailsQueuedExactlyOnce) {
    makeServer(HttpServerConfig{.workerNum = 2});
    m_server->Get("/slow", [](const HttpRequest&, HttpResponse& r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        r.setBody(std::string("ok"));
    });
    m_server->Start();
    makePool(HttpPoolConfig{.maxConnections = 1});

    std::atomic<int> fires{0};
    std::atomic<int> nullopts{0};
    auto cb = [&](std::optional<HttpResponse> r) {
        ++fires;
        if (!r.has_value()) ++nullopts;
    };
    m_pool->Get("/slow", cb);  // 在途
    m_pool->Get("/slow", cb);  // 排队
    m_pool->Get("/slow", cb);  // 排队
    std::atomic<bool> done{false};
    m_pool->Stop([&] { done = true; });
    ASSERT_TRUE(WaitFor([&] { return fires.load() == 3; }, std::chrono::milliseconds(10000)));
    EXPECT_EQ(nullopts.load(), 3);
    EXPECT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
}

/// [界] 无连接 park + Stop（审查修复）：parked cb nullopt exactly-once，done 须在实例清理后触发（析构安全）
TEST_F(HttpConnectionPoolTest, StopWithParkedRequestNoConnection) {
    // 不启服务端：请求 park 于 handoff（Connector 退避重试中），连接从未建立。
    // 局部池：done 触发后即析构——done 必须已含实例 Stop 清理完成（否则 TcpClient 析构违约）
    auto pool = std::make_unique<HttpConnectionPool>(
        m_client_loop.loop(), "127.0.0.1:" + std::to_string(m_port), HttpPoolConfig{.maxConnections = 1});
    pool->Start();
    std::atomic<int> fires{0};
    std::atomic<bool> is_null{false};
    pool->Get("/parked", [&](std::optional<HttpResponse> r) {
        ++fires;
        is_null = !r.has_value();
    });
    std::atomic<bool> done{false};
    pool->Stop([&] { done = true; });
    ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
    EXPECT_EQ(fires.load(), 1);
    EXPECT_TRUE(is_null.load());
    pool.reset();
    RunInLoopSync(m_client_loop.loop(), [] {});
}

/// [正] 顺序请求复用同连接（H5.2 故事 9：keep-alive 不每请求建连）
TEST_F(HttpConnectionPoolTest, SequentialRequestsReuseConnection) {
    makeServer();
    std::mutex mu;
    std::set<std::string> peers;
    m_server->Get("/one", [&](const HttpRequest& req, HttpResponse& r) {
        std::lock_guard<std::mutex> lk(mu);
        peers.insert(std::string(req.getRemoteAddr()));
        r.setBody(std::string("ok"));
    });
    m_server->Start();
    makePool(HttpPoolConfig{.maxConnections = 2});
    for (int i = 0; i < 2; ++i) {
        auto l = std::make_shared<CallbackLatch>();
        m_pool->Get("/one", [l](std::optional<HttpResponse> r) { l->fire(std::move(r)); });
        auto r = l->wait();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->getStatus(), HttpStatus::kOK);
    }
    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(peers.size(), 1u);
}
