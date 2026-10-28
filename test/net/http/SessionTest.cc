//***********************************************
// name      : SessionTest.cc
// brief     : Session 管理测试（建立/Cookie 回写/过期/洪水防护/并发）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/http/HttpServer.h"
#include "net/http/Session.h"

using namespace bus;
using namespace bus::http;
using namespace bus::test;

namespace {

/// @brief 构造带 Cookie 的请求
HttpRequest MakeReqWithCookie(std::string_view cookie = {}) {
    HttpRequest req;
    req.setMethod(HttpMethod::kGET);
    req.setPath("/s");
    if (!cookie.empty()) {
        std::string c(cookie);
        req.addHeaderRaw("Cookie", std::string_view(c));
    }
    req.finishCookies();
    return req;
}

/// @brief 从响应 Dump 序列化产物提取 Set-Cookie 的值段（Cookie 独立容器，须走 Dump）
std::string ExtractCookieValue(const std::string& dump) {
    size_t pos = dump.find("Set-Cookie: ");
    if (pos == std::string::npos) {
        return "";
    }
    size_t eq = dump.find('=', pos);
    size_t semi = dump.find(';', eq);
    return dump.substr(eq + 1, semi == std::string::npos ? dump.find("\r\n", eq) - eq - 1 : semi - eq - 1);
}

}  // namespace

/// [正] 新建会话回写 Cookie
TEST(SessionTest, CreateSessionSetsCookie) {
    SessionManager mgr;
    HttpRequest req = MakeReqWithCookie();
    auto s = mgr.getSession(req);
    ASSERT_NE(s, nullptr);
    HttpResponse resp;
    SessionManager::attach(resp, s, SessionConfig{});
    // Set-Cookie 经 Dump 序列化输出（独立容器，getHeader 不可见）——从序列化产物断言
    HttpRequest probe = MakeReqWithCookie();
    std::string dump = resp.Dump(probe);
    EXPECT_EQ(ExtractCookieValue(dump), s->getId());
    size_t pos = dump.find("Set-Cookie: ");
    ASSERT_NE(pos, std::string::npos);
    std::string line = dump.substr(pos, dump.find("\r\n", pos) - pos);
    EXPECT_NE(line.find("HttpOnly"), std::string::npos);
    EXPECT_NE(line.find("SameSite=Lax"), std::string::npos);
    EXPECT_NE(line.find("Max-Age=1800"), std::string::npos);
}

/// [正] 携带 Cookie 保持同 id
TEST(SessionTest, CookieRoundTripKeepsId) {
    SessionManager mgr;
    auto s1 = mgr.getSession(MakeReqWithCookie());
    ASSERT_NE(s1, nullptr);
    auto s2 = mgr.getSession(MakeReqWithCookie("CODEBUSID=" + s1->getId()));
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->getId(), s1->getId());
    EXPECT_EQ(mgr.size(), 1u);
}

/// [正] 属性跨请求存活
TEST(SessionTest, AttributesSurviveAcrossRequests) {
    SessionManager mgr;
    auto s1 = mgr.getSession(MakeReqWithCookie());
    s1->setAttribute("user", "tom");
    auto s2 = mgr.getSession(MakeReqWithCookie("CODEBUSID=" + s1->getId()));
    auto v = s2->getAttribute("user");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "tom");
    s2->eraseAttribute("user");
    EXPECT_FALSE(s2->getAttribute("user").has_value());
}

/// [正] findSession 不新建
TEST(SessionTest, FindSessionDoesNotCreate) {
    SessionManager mgr;
    EXPECT_EQ(mgr.findSession(MakeReqWithCookie()), nullptr);
    EXPECT_EQ(mgr.size(), 0u);
}

/// [正] 惰性过期重建
TEST(SessionTest, LazyExpiryRecreates) {
    SessionManager mgr(SessionConfig{.timeoutSec = 1});
    auto s1 = mgr.getSession(MakeReqWithCookie());
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    auto s2 = mgr.getSession(MakeReqWithCookie("CODEBUSID=" + s1->getId()));
    ASSERT_NE(s2, nullptr);
    EXPECT_NE(s2->getId(), s1->getId());  // 过期重建
}

/// [正] sweep 回收过期条目
TEST(SessionTest, SweepReclaimsExpired) {
    SessionManager mgr(SessionConfig{.timeoutSec = 1});
    mgr.getSession(MakeReqWithCookie());
    ASSERT_EQ(mgr.size(), 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    EXPECT_EQ(mgr.sweep(), 1u);  // 回收
    EXPECT_EQ(mgr.size(), 0u);
}

/// [正] 伪造 id 未命中重建
TEST(SessionTest, ForgedIdRecreates) {
    SessionManager mgr;
    auto s = mgr.getSession(MakeReqWithCookie("CODEBUSID=deadbeefdeadbeefdeadbeefdeadbeef"));
    ASSERT_NE(s, nullptr);
    EXPECT_NE(s->getId(), "deadbeefdeadbeefdeadbeefdeadbeef");
}

/// [界] 洪水超限拒建
TEST(SessionTest, MaxSessionsFlood503) {
    SessionManager mgr(SessionConfig{.timeoutSec = 60, .maxSessions = 2});
    EXPECT_NE(mgr.getSession(MakeReqWithCookie()), nullptr);
    EXPECT_NE(mgr.getSession(MakeReqWithCookie()), nullptr);
    EXPECT_EQ(mgr.getSession(MakeReqWithCookie()), nullptr);  // 超限（调用方 503）
    EXPECT_EQ(mgr.size(), 2u);
}

/// [正] manager 生命周期独立于 server
TEST(SessionTest, ManagerOutlivesServer) {
    auto mgr = std::make_shared<SessionManager>();
    {
        LoopThread lt;
        uint16_t port = GetFreePort();
        HttpServer server(lt.loop(), "127.0.0.1:" + std::to_string(port), 0);
        server.addFilter(std::make_shared<SessionFilter>(mgr));
        server.Get("/s", [mgr](const HttpRequest& req, HttpResponse& r) {
            auto s = mgr->getSession(req);
            if (!s) {
                r.SendError(HttpStatus::kSERVICE_UNAVAILABLE, "session limit");
                return;
            }
            int hits = 0;
            {
                auto v = s->getAttribute("hits");
                hits = v ? atoi(std::string(*v).c_str()) + 1 : 1;
            }
            s->setAttribute("hits", std::to_string(hits));
            SessionManager::attach(r, s, SessionConfig{});
            r.setBody("hits=" + std::to_string(hits));
        });
        server.Start();
        std::atomic<bool> done{false};
        server.Stop([&] { done = true; });
        ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
    }  // server + loop 均析构
    auto s = mgr->getSession(MakeReqWithCookie());  // manager 照常工作
    EXPECT_NE(s, nullptr);
    SUCCEED();
}
