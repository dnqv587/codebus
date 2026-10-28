//***********************************************
// name      : ServletDispatcherTest.cc
// brief     : Servlet 容器与 Filter 链测试（路由/方法分派/短路/异常兜底）
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "net/http/ServletDispatcher.h"

using namespace bus;
using namespace bus::http;

namespace {

/// @brief 构造已装配路径/方法的测试请求
HttpRequest MakeRequest(HttpMethod method, std::string_view path) {
    HttpRequest req;
    req.setMethod(method);
    req.setPath(std::string(path));
    return req;
}

/// @brief 触发时把标记写入 body 的 Servlet
class TagServlet : public HttpServlet {
 public:
    explicit TagServlet(std::string tag) : m_tag(std::move(tag)) {}
    void doGet(const HttpRequest&, HttpResponse& resp) override {
        resp.setBody(m_tag);
    }
    void doPost(const HttpRequest&, HttpResponse& resp) override {
        resp.setBody(m_tag);
    }

 private:
    std::string m_tag;
};

/// @brief 记录触发顺序的过滤器；pass_through=false 时短路并回 403
class TagFilter : public Filter {
 public:
    TagFilter(std::string tag, std::string* log, bool passThrough = true)
        : m_tag(std::move(tag)),
          m_log(log),
          m_pass(passThrough) {}
    void doFilter(const HttpRequest& req, HttpResponse& resp, FilterChain& chain) override {
        m_log->append(m_tag);
        if (m_pass) {
            chain.doFilter(req, resp);  // 放行下一环
        } else {
            resp.SendError(HttpStatus::kFORBIDDEN, m_tag + " blocked");
        }
    }

 private:
    std::string m_tag;
    std::string* m_log;
    bool m_pass;
};

/// @brief 抛异常的过滤器（500 兜底用例）
class ThrowingFilter : public Filter {
 public:
    void doFilter(const HttpRequest&, HttpResponse&, FilterChain&) override {
        throw std::runtime_error("filter boom");
    }
};


/// [正] 模式参数提取：/user/{id}（U2）
TEST(ServletDispatcherTest, PatternParamExtract) {
    ServletDispatcher d;
    std::string got;
    d.addPatternServlet("/user/{id}",
                        std::make_shared<FunctionServlet>([&](const HttpRequest& req, HttpResponse& r) {
                            got = std::string(req.getPathParam("id").value_or("?"));
                            r.setBody(std::string("ok"));
                        }));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/user/42"), resp);
    EXPECT_EQ(got, "42");
}

/// [正] 多参数段
TEST(ServletDispatcherTest, PatternMultiParams) {
    ServletDispatcher d;
    std::string x, y;
    d.addPatternServlet("/a/{x}/b/{y}",
                        std::make_shared<FunctionServlet>([&](const HttpRequest& req, HttpResponse& r) {
                            x = std::string(req.getPathParam("x").value_or("?"));
                            y = std::string(req.getPathParam("y").value_or("?"));
                        }));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/a/1/b/2"), resp);
    EXPECT_EQ(x, "1");
    EXPECT_EQ(y, "2");
}

/// [正] 精确优先于模式
TEST(ServletDispatcherTest, ExactBeatsPattern) {
    ServletDispatcher d;
    d.addPatternServlet("/user/{id}", std::make_shared<TagServlet>("pattern"));
    d.addExactServlet("/user/me", std::make_shared<TagServlet>("exact"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/user/me"), resp);
    EXPECT_EQ(resp.getBody(), "exact");
    HttpResponse resp2;
    d.Handle(MakeRequest(HttpMethod::kGET, "/user/9"), resp2);
    EXPECT_EQ(resp2.getBody(), "pattern");  // 其余仍走模式
}

/// [正] 模式间静态段多者胜
TEST(ServletDispatcherTest, PatternStaticWeightWins) {
    ServletDispatcher d;
    d.addPatternServlet("/user/{id}/{sub}", std::make_shared<TagServlet>("loose"));
    d.addPatternServlet("/user/{id}/orders", std::make_shared<TagServlet>("tight"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/user/7/orders"), resp);
    EXPECT_EQ(resp.getBody(), "tight");
}

/// [正] 尾段 * 多段捕获（段边界）
TEST(ServletDispatcherTest, TailWildcard) {
    ServletDispatcher d;
    d.addPatternServlet("/static/*", std::make_shared<TagServlet>("assets"));
    HttpResponse hit;
    d.Handle(MakeRequest(HttpMethod::kGET, "/static/a/b.js"), hit);
    EXPECT_EQ(hit.getBody(), "assets");
    HttpResponse miss;
    d.Handle(MakeRequest(HttpMethod::kGET, "/staticx"), miss);
    EXPECT_EQ(miss.getStatus(), HttpStatus::kNOT_FOUND);  // 段边界不命中
}

/// [正] 模式优先于前缀（现有注册零变化）
TEST(ServletDispatcherTest, PatternPrefixCoexist) {
    ServletDispatcher d;
    d.addPrefixServlet("/api", std::make_shared<TagServlet>("prefix"));
    d.addPatternServlet("/api/{v}/info", std::make_shared<TagServlet>("pattern"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/api/v2/info"), resp);
    EXPECT_EQ(resp.getBody(), "pattern");
    HttpResponse resp2;
    d.Handle(MakeRequest(HttpMethod::kGET, "/api/other"), resp2);
    EXPECT_EQ(resp2.getBody(), "prefix");
}

/// [界] 非法模式拒绝（空参数名/非尾 *）
TEST(ServletDispatcherTest, InvalidPatternRejected) {
    ServletDispatcher d;
    d.addPatternServlet("/x/{}", std::make_shared<TagServlet>("bad1"));      // 空参数名
    d.addPatternServlet("/a/*/b", std::make_shared<TagServlet>("bad2"));     // 非尾 *
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/x/1"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kNOT_FOUND);  // 均未注册
}

/// [界] 同模式重注册后者胜
TEST(ServletDispatcherTest, SamePatternReregister) {
    ServletDispatcher d;
    d.addPatternServlet("/u/{id}", std::make_shared<TagServlet>("first"));
    d.addPatternServlet("/u/{id}", std::make_shared<TagServlet>("second"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/u/1"), resp);
    EXPECT_EQ(resp.getBody(), "second");
}

/// [正] dot-segment 规范化：/a/../b 按 /b 路由；越界 400
TEST(ServletDispatcherTest, DotSegmentNormalized) {
    ServletDispatcher d;
    d.addExactServlet("/b", std::make_shared<TagServlet>("B"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/a/../b"), resp);
    EXPECT_EQ(resp.getBody(), "B");
    HttpResponse bad;
    d.Handle(MakeRequest(HttpMethod::kGET, "/../x"), bad);
    EXPECT_EQ(bad.getStatus(), HttpStatus::kBAD_REQUEST);
}

/// [界] 冻结后模式注册忽略
TEST(ServletDispatcherTest, FrozenPatternRegisterIgnored) {
    ServletDispatcher d;
    d.freeze();
    d.addPatternServlet("/f/{id}", std::make_shared<TagServlet>("F"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/f/1"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kNOT_FOUND);
}

/// [正] 路径 Filter 作用域与顺序：全局 → 命中路径链 → Servlet
TEST(ServletDispatcherTest, PathFilterOrderAndScope) {
    ServletDispatcher d;
    std::string log;
    d.addFilter(std::make_shared<TagFilter>("G", &log));                    // 全局
    d.addFilter("/sec/*", std::make_shared<TagFilter>("P", &log));          // 路径
    d.addExactServlet("/sec/a", std::make_shared<TagServlet>("S"));
    d.addExactServlet("/open", std::make_shared<TagServlet>("O"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/sec/a"), resp);
    EXPECT_EQ(log, "GP");          // 全局→路径链
    EXPECT_EQ(resp.getBody(), "S");
    log.clear();
    HttpResponse resp2;
    d.Handle(MakeRequest(HttpMethod::kGET, "/open"), resp2);
    EXPECT_EQ(log, "G");           // 未命中路径链跳过
    EXPECT_EQ(resp2.getBody(), "O");
}

}  // namespace

/// [正] 精确匹配优先于前缀
TEST(ServletDispatcherTest, ExactBeatsPrefix) {
    ServletDispatcher d;
    d.addPrefixServlet("/", std::make_shared<TagServlet>("prefix-root"));
    d.addExactServlet("/api", std::make_shared<TagServlet>("exact-api"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/api"), resp);
    EXPECT_EQ(resp.getBody(), "exact-api");
}

/// [正] 最长前缀胜出
TEST(ServletDispatcherTest, LongestPrefixWins) {
    ServletDispatcher d;
    d.addPrefixServlet("/a", std::make_shared<TagServlet>("short"));
    d.addPrefixServlet("/a/b", std::make_shared<TagServlet>("long"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/a/b/c"), resp);
    EXPECT_EQ(resp.getBody(), "long");
    HttpResponse resp2;
    d.Handle(MakeRequest(HttpMethod::kGET, "/a/x"), resp2);
    EXPECT_EQ(resp2.getBody(), "short");
}

/// [界] 前缀段边界：/api 不匹配 /apiv2
TEST(ServletDispatcherTest, PrefixSegmentBoundary) {
    ServletDispatcher d;
    d.addPrefixServlet("/api", std::make_shared<TagServlet>("api"));
    HttpResponse miss;
    d.Handle(MakeRequest(HttpMethod::kGET, "/apiv2"), miss);
    EXPECT_EQ(miss.getStatus(), HttpStatus::kNOT_FOUND);  // 边界外不命中

    HttpResponse hit1;
    d.Handle(MakeRequest(HttpMethod::kGET, "/api"), hit1);
    EXPECT_EQ(hit1.getBody(), "api");
    HttpResponse hit2;
    d.Handle(MakeRequest(HttpMethod::kGET, "/api/v2"), hit2);
    EXPECT_EQ(hit2.getBody(), "api");
}

/// [界] 同前缀重复注册：后者生效
TEST(ServletDispatcherTest, SamePrefixReregisterOverwrites) {
    ServletDispatcher d;
    d.addPrefixServlet("/x", std::make_shared<TagServlet>("first"));
    d.addPrefixServlet("/x", std::make_shared<TagServlet>("second"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/x/1"), resp);
    EXPECT_EQ(resp.getBody(), "second");
}

/// [正] 未命中走 NotFound 兜底 404
TEST(ServletDispatcherTest, NoMatchYieldsNotFound) {
    ServletDispatcher d;
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/nothing"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kNOT_FOUND);
}

/// [正] HttpServlet 方法分派：未覆盖方法 405
TEST(ServletDispatcherTest, HttpServletMethodDispatch) {
    class OnlyGetServlet : public HttpServlet {
     public:
        void doGet(const HttpRequest&, HttpResponse& resp) override {
            resp.setBody(std::string("g"));
        }
    };
    ServletDispatcher d;
    d.addExactServlet("/only-get", std::make_shared<OnlyGetServlet>());
    HttpResponse ok;
    d.Handle(MakeRequest(HttpMethod::kGET, "/only-get"), ok);
    EXPECT_EQ(ok.getBody(), "g");
    EXPECT_EQ(ok.getStatus(), HttpStatus::kOK);

    HttpResponse denied;
    d.Handle(MakeRequest(HttpMethod::kPOST, "/only-get"), denied);
    EXPECT_EQ(denied.getStatus(), HttpStatus::kMETHOD_NOT_ALLOWED);  // 405
    ASSERT_TRUE(denied.getHeader("Allow").has_value());
}

/// [正] doHead 默认转调 doGet
TEST(ServletDispatcherTest, DoHeadFallsBackToDoGet) {
    static int get_hits = 0;
    class HeadServlet : public HttpServlet {
     public:
        void doGet(const HttpRequest&, HttpResponse& resp) override {
            ++get_hits;
            resp.setBody(std::string("body"));
        }
    };
    ServletDispatcher d;
    d.addExactServlet("/h", std::make_shared<HeadServlet>());
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kHEAD, "/h"), resp);
    EXPECT_EQ(get_hits, 1);             // doGet 被执行
    EXPECT_EQ(resp.getBody(), "body");  // 剥 body 是 Dump 依据 HEAD 的职责
}

/// [正] OPTIONS 回 Allow 清单
TEST(ServletDispatcherTest, DoOptionsAllowList) {
    ServletDispatcher d;
    d.addExactServlet("/opt", std::make_shared<TagServlet>("x"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kOPTIONS, "/opt"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kOK);
    ASSERT_TRUE(resp.getHeader("Allow").has_value());
    EXPECT_NE(resp.getHeader("Allow")->find("GET"), std::string_view::npos);
    EXPECT_NE(resp.getHeader("Allow")->find("POST"), std::string_view::npos);
}

/// [正] FunctionServlet 适配 lambda
TEST(ServletDispatcherTest, FunctionServletAdapts) {
    ServletDispatcher d;
    d.addExactServlet("/fn", std::make_shared<FunctionServlet>([](const HttpRequest& req, HttpResponse& resp) {
                          resp.setBody("fn:" + std::string(req.getPath()));
                      }));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/fn"), resp);
    EXPECT_EQ(resp.getBody(), "fn:/fn");
}

/// [正] Filter 链顺序：A → B → Servlet
TEST(ServletDispatcherTest, FilterChainOrder) {
    ServletDispatcher d;
    std::string log;
    d.addFilter(std::make_shared<TagFilter>("A", &log));
    d.addFilter(std::make_shared<TagFilter>("B", &log));
    d.addExactServlet("/s", std::make_shared<TagServlet>("S"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/s"), resp);
    EXPECT_EQ(log, "AB");
    EXPECT_EQ(resp.getBody(), "S");
}

/// [正] Filter 短路：不放行则 Servlet 不执行、回 403
TEST(ServletDispatcherTest, FilterShortCircuits) {
    ServletDispatcher d;
    std::string log;
    d.addFilter(std::make_shared<TagFilter>("A", &log));
    d.addFilter(std::make_shared<TagFilter>("B", &log, /*passThrough=*/false));
    d.addExactServlet("/s", std::make_shared<TagServlet>("S"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/s"), resp);
    EXPECT_EQ(log, "AB");  // B 被触发（A 放行后）
    EXPECT_EQ(resp.getStatus(), HttpStatus::kFORBIDDEN);
    EXPECT_EQ(resp.getBody(), "B blocked");  // Servlet 未执行，body 为短路错误页
}

/// [界] Servlet 异常 → 500 兜底
TEST(ServletDispatcherTest, ServletExceptionYields500) {
    ServletDispatcher d;
    d.addExactServlet("/boom", std::make_shared<FunctionServlet>(
                                   [](const HttpRequest&, HttpResponse&) { throw std::runtime_error("boom"); }));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/boom"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kINTERNAL_SERVER_ERROR);
    EXPECT_NE(resp.getBody().find("boom"), std::string_view::npos);
}

/// [界] Filter 异常 → 500 兜底
TEST(ServletDispatcherTest, FilterExceptionYields500) {
    ServletDispatcher d;
    d.addFilter(std::make_shared<ThrowingFilter>());
    d.addExactServlet("/s", std::make_shared<TagServlet>("S"));
    HttpResponse resp;
    d.Handle(MakeRequest(HttpMethod::kGET, "/s"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kINTERNAL_SERVER_ERROR);
    EXPECT_NE(resp.getBody().find("filter boom"), std::string_view::npos);
}
