//***********************************************
// name      : HttpMessageTest.cc
// brief     : HttpMessage 协议对象层测试（HeaderMap/Cookie/Request/Response/Dump）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <cctype>
#include <clocale>
#include <fstream>
#include <unistd.h>
#include "net/http/HttpMessage.h"

using namespace bus::http;

/// [正] 大小写不敏感命中
TEST(HttpMessageTest, HeaderMapCaseInsensitive) {
    HeaderMap h;
    h.addHeader("Content-Type", "text/html");
    ASSERT_TRUE(h.getHeader("content-type").has_value());
    EXPECT_EQ(*h.getHeader("CONTENT-TYPE"), "text/html");
    EXPECT_FALSE(h.getHeader("X-Miss").has_value());
    EXPECT_TRUE(h.hasHeader("Content-type"));
    EXPECT_EQ(h.size(), 1u);
    h.clear();
    EXPECT_TRUE(h.getHeader("content-type") == std::nullopt);
}

/// [正] setHeader 覆盖语义：删全部同名后追加（保尾序）
TEST(HttpMessageTest, HeaderMapSetOverrides) {
    HeaderMap h;
    h.addHeader("X-A", "1");
    h.setHeader("x-a", "2");  // 删 "1" 追加 "2"
    h.addHeader("X-A", "3");
    auto all = h.getHeaders("X-A");
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0], "2");
    EXPECT_EQ(all[1], "3");
    EXPECT_EQ(*h.getHeader("x-a"), "2");  // 首个
    h.setHeader("X-A", "9");
    EXPECT_EQ(h.getHeaders("X-A").size(), 1u);
    EXPECT_EQ(*h.getHeader("x-a"), "9");
    EXPECT_EQ(h.size(), 1u);
}

/// [正] 请求 Cookie 解析：';' 切分、trim、无 '=' 段跳过
TEST(HttpMessageTest, CookieParse) {
    auto cs = ParseRequestCookies("a=1; b=2");
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].name, "a");
    EXPECT_EQ(cs[0].value, "1");
    EXPECT_EQ(cs[1].name, "b");
    EXPECT_EQ(cs[1].value, "2");

    cs = ParseRequestCookies("  a = 1 ; b=2");
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].name, "a");
    EXPECT_EQ(cs[0].value, "1");

    cs = ParseRequestCookies("novalue; a=1");
    ASSERT_EQ(cs.size(), 1u);  // 无 '=' 段跳过
    cs = ParseRequestCookies("; ;");
    EXPECT_TRUE(cs.empty());
    cs = ParseRequestCookies("");
    EXPECT_TRUE(cs.empty());
}

/// [正] Set-Cookie 序列化：默认仅 Path；全属性按序
TEST(HttpMessageTest, CookieSerialize) {
    Cookie c{.name = "sid", .value = "abc"};
    EXPECT_EQ(SerializeSetCookie(c), "sid=abc; Path=/");

    Cookie full{.name = "sid",
                .value = "abc",
                .path = "/app",
                .domain = "example.com",
                .expires = "Wed, 21 Oct 2026 07:28:00 GMT",
                .maxAge = 3600,
                .secure = true,
                .httpOnly = true,
                .sameSite = "Lax"};
    EXPECT_EQ(SerializeSetCookie(full),
              "sid=abc; Path=/app; Domain=example.com; "
              "Expires=Wed, 21 Oct 2026 07:28:00 GMT; Max-Age=3600; SameSite=Lax; Secure; HttpOnly");
}

/// [正] 请求装配与只读访问
TEST(HttpMessageTest, RequestAccessors) {
    HttpRequest r;
    r.setMethod(HttpMethod::kPOST);
    r.setMajor(1);
    r.setMinor(0);
    r.appendUrl("/a%20b?k=v");
    r.setPath("/a b");
    r.setQuery("k=v");
    r.addHeaderRaw("Host", "codebus");
    r.appendBody("data");
    r.setKeepAlive(false);
    r.setChunked(true);
    r.setExpectContinue(true);
    r.finishParams();

    EXPECT_EQ(r.getMethod(), HttpMethod::kPOST);
    EXPECT_EQ(r.getVersion(), "HTTP/1.0");
    EXPECT_EQ(r.getRawUrl(), "/a%20b?k=v");
    EXPECT_EQ(r.getPath(), "/a b");
    EXPECT_EQ(r.getQuery(), "k=v");
    ASSERT_TRUE(r.getHeader("host").has_value());
    EXPECT_EQ(*r.getHeader("host"), "codebus");
    EXPECT_EQ(r.getBody(), "data");
    EXPECT_EQ(r.getContentLength(), 4u);
    EXPECT_FALSE(r.isKeepAlive());
    EXPECT_TRUE(r.isChunked());
    EXPECT_TRUE(r.isExpectContinue());
    ASSERT_TRUE(r.getParam("k").has_value());
    EXPECT_EQ(*r.getParam("k"), "v");
    EXPECT_FALSE(r.getParam("miss").has_value());

    /// [正] Swap 零拷贝交接与 clear 复位
    HttpRequest o;
    r.Swap(o);
    EXPECT_TRUE(r.getBody().empty());
    EXPECT_EQ(o.getBody(), "data");
    o.clear();
    EXPECT_EQ(o.getMethod(), HttpMethod::kGET);
    EXPECT_EQ(o.getVersion(), "HTTP/1.1");
    EXPECT_TRUE(o.isKeepAlive());
    EXPECT_TRUE(o.getBody().empty());
}

/// [正] query 参数解码（'+' 按空格、%XX）
TEST(HttpMessageTest, RequestParamDecode) {
    HttpRequest r;
    r.setQuery("a=1+2&b=%48%69");
    r.finishParams();
    ASSERT_TRUE(r.getParam("a").has_value());
    EXPECT_EQ(*r.getParam("a"), "1 2");
    ASSERT_TRUE(r.getParam("b").has_value());
    EXPECT_EQ(*r.getParam("b"), "Hi");
    EXPECT_EQ(r.getParams("a").size(), 1u);
}

/// [正] 基本序列化快照
TEST(HttpMessageTest, DumpBasic) {
    HttpRequest req;
    req.setMethod(HttpMethod::kGET);
    HttpResponse resp;
    resp.setHeader("X-Custom", "v");
    resp.setBody(std::string("hello"));
    std::string out = resp.Dump(req);
    EXPECT_TRUE(out.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(out.find("Server: codebus-http\r\n"), std::string::npos);
    EXPECT_NE(out.find("X-Custom: v\r\n"), std::string::npos);
    EXPECT_NE(out.find("Content-Length: 5\r\n"), std::string::npos);
    EXPECT_NE(out.find("Connection: keep-alive\r\n"), std::string::npos);
    EXPECT_TRUE(out.ends_with("\r\n\r\nhello"));
}

/// [正] 状态行 HTTP 版本跟随请求（1.0 请求回 HTTP/1.0）
TEST(HttpMessageTest, DumpVersionFollowsRequest) {
    HttpRequest req;
    req.setMinor(0);
    HttpResponse resp;
    EXPECT_TRUE(resp.Dump(req).starts_with("HTTP/1.0 200 OK\r\n"));
}

/// [正] 请求 Connection: close 或响应 setKeepAlive(false) → Connection: close
TEST(HttpMessageTest, DumpClose) {
    HttpRequest req;
    req.setKeepAlive(false);
    HttpResponse resp;
    EXPECT_NE(resp.Dump(req).find("Connection: close\r\n"), std::string::npos);

    HttpRequest req2;
    HttpResponse resp2;
    resp2.setKeepAlive(false);
    EXPECT_NE(resp2.Dump(req2).find("Connection: close\r\n"), std::string::npos);
}

/// [正] HEAD：输出应有 Content-Length 但不输出 body 字节
TEST(HttpMessageTest, DumpHeadOmitsBody) {
    HttpRequest req;
    req.setMethod(HttpMethod::kHEAD);
    HttpResponse resp;
    resp.setBody(std::string("hello"));
    std::string out = resp.Dump(req);
    EXPECT_NE(out.find("Content-Length: 5\r\n"), std::string::npos);
    EXPECT_TRUE(out.ends_with("\r\n\r\n"));
}

/// [正] 204 无 body 且不携带 Content-Length（RFC 7230 §3.3.2；304 保留）
TEST(HttpMessageTest, Dump204NoBody) {
    HttpRequest req;
    HttpResponse resp;
    resp.setStatus(HttpStatus::kNO_CONTENT);
    resp.setBody(std::string("ignored"));
    std::string out = resp.Dump(req);
    EXPECT_TRUE(out.starts_with("HTTP/1.1 204 No Content\r\n"));
    EXPECT_EQ(out.find("Content-Length:"), std::string::npos);  // 1xx/204 禁带 CL
    EXPECT_TRUE(out.ends_with("\r\n\r\n"));                     // 无 body 字节

    resp.setStatus(HttpStatus::kNOT_MODIFIED);  // 304 保留 CL（实体大小提示）
    out = resp.Dump(req);
    EXPECT_NE(out.find("Content-Length: 7\r\n"), std::string::npos);
}

/// [正] 多条 Set-Cookie 各占一行
TEST(HttpMessageTest, DumpSetCookieMulti) {
    HttpRequest req;
    HttpResponse resp;
    Cookie a{.name = "a", .value = "1"};
    Cookie b{.name = "b", .value = "2"};
    resp.addCookie(a);
    resp.addCookie(b);
    std::string out = resp.Dump(req);
    EXPECT_NE(out.find("Set-Cookie: a=1; Path=/\r\n"), std::string::npos);
    EXPECT_NE(out.find("Set-Cookie: b=2; Path=/\r\n"), std::string::npos);
    size_t pos = 0, count = 0;
    while ((pos = out.find("Set-Cookie: ", pos)) != std::string::npos) {
        ++count;
        pos += 12;
    }
    EXPECT_EQ(count, 2u);
}

/// [界] 托管头被跳过（用户设 Content-Length/Transfer-Encoding 无效）
TEST(HttpMessageTest, DumpManagedHeadersSkipped) {
    HttpRequest req;
    HttpResponse resp;
    resp.setHeader("Content-Length", "999");
    resp.setHeader("Transfer-Encoding", "chunked");
    resp.setBody(std::string("hello"));
    std::string out = resp.Dump(req);
    EXPECT_EQ(out.find("Content-Length: 999"), std::string::npos);
    EXPECT_EQ(out.find("Transfer-Encoding"), std::string::npos);
    EXPECT_NE(out.find("Content-Length: 5\r\n"), std::string::npos);
}

/// [正] 错误页：状态行 + text/plain + 默认 body
TEST(HttpMessageTest, SendError) {
    HttpRequest req;
    HttpResponse resp;
    resp.SendError(HttpStatus::kNOT_FOUND);
    std::string out = resp.Dump(req);
    EXPECT_TRUE(out.starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_NE(out.find("Content-Type: text/plain; charset=utf-8\r\n"), std::string::npos);
    EXPECT_TRUE(out.ends_with("\r\n\r\n404 Not Found"));

    resp.SendError(HttpStatus::kBAD_REQUEST, "bad syntax");
    EXPECT_EQ(resp.getBody(), "bad syntax");
}

/// [正] 重定向：302 + Location + html body
TEST(HttpMessageTest, SendRedirect) {
    HttpRequest req;
    HttpResponse resp;
    resp.SendRedirect("/login");
    std::string out = resp.Dump(req);
    EXPECT_TRUE(out.starts_with("HTTP/1.1 302 Found\r\n"));
    EXPECT_NE(out.find("Location: /login\r\n"), std::string::npos);
    EXPECT_NE(out.find("<a href=\"/login\">"), std::string::npos);
}

/// [界] 响应头 CRLF 注入拒绝（P1 修复锁定：CR/LF/NUL 整条丢弃）
TEST(HttpMessageTest, CrlfInjectionRejected) {
    HttpRequest req;
    HttpResponse resp;
    resp.setHeader("X-A", "v\r\nX-Injected: 1");
    resp.addHeader("X-B\rv", "x");
    resp.setBody(std::string("ok"));
    std::string out = resp.Dump(req);
    EXPECT_EQ(out.find("X-Injected"), std::string::npos);
    EXPECT_EQ(out.find("X-A:"), std::string::npos);  // 含 CR/LF 的整条被拒
    EXPECT_EQ(out.find("X-B"), std::string::npos);
    EXPECT_NE(out.find("Content-Length: 2"), std::string::npos);  // 正常部分不受影响

    HttpResponse redir;
    redir.SendRedirect("/x\r\nSet-Cookie: pwn=1");
    std::string out2 = redir.Dump(req);
    // Location 头整条被拒（含 CR/LF）；body 中残留的是无害字面量——注入形态
    // （头区的独立 Set-Cookie 行）不得出现：仅检查头部区域
    size_t hdr_end2 = out2.find("\r\n\r\n");
    ASSERT_NE(hdr_end2, std::string::npos);
    std::string headers2 = out2.substr(0, hdr_end2);
    EXPECT_EQ(headers2.find("Set-Cookie:"), std::string::npos);
    EXPECT_EQ(headers2.find("Location:"), std::string::npos);
}

/// [界] Cookie 名/值注入拒绝（序列化出口防线）
TEST(HttpMessageTest, CookieInjectionRejected) {
    HttpRequest req;
    HttpResponse resp;
    Cookie bad{.name = "s\r\nX-I: 1", .value = "v"};
    Cookie good{.name = "ok", .value = "1"};
    resp.addCookie(bad);
    resp.addCookie(good);
    std::string out = resp.Dump(req);
    EXPECT_EQ(out.find("X-I:"), std::string::npos);
    EXPECT_EQ(out.find("Set-Cookie: s"), std::string::npos);
    EXPECT_NE(out.find("Set-Cookie: ok=1"), std::string::npos);  // 合法 cookie 正常
}

/// [正] 多个 Cookie 头合并解析（RFC 允许分段）
TEST(HttpMessageTest, MultipleCookieHeaders) {
    HttpRequest r;
    r.addHeaderRaw("Cookie", "a=1");
    r.addHeaderRaw("cookie", "b=2");
    r.finishCookies();
    EXPECT_EQ(r.getCookies().size(), 2u);
    ASSERT_TRUE(r.getCookie("a").has_value());
    EXPECT_EQ(*r.getCookie("a"), "1");
    ASSERT_TRUE(r.getCookie("b").has_value());
    EXPECT_EQ(*r.getCookie("b"), "2");
}

/// [正] Date 头不受 LC_TIME locale 影响（Q33：查表生成星期/月份）
TEST(HttpMessageTest, DateLocaleInvariant) {
    HttpRequest req;
    HttpResponse resp;
    std::string date_en;
    {
        std::string out = resp.Dump(req);
        size_t pos = out.find("Date: ");
        ASSERT_NE(pos, std::string::npos);
        date_en = out.substr(pos + 6, out.find("\r\n", pos) - pos - 6);
    }
    std::string old = setlocale(LC_ALL, nullptr);
    setlocale(LC_ALL, "zh_CN.UTF-8");  // 若环境无此 locale，setlocale 返回 nullptr 且不生效
    std::string out2 = resp.Dump(req);
    size_t pos2 = out2.find("Date: ");
    ASSERT_NE(pos2, std::string::npos);
    std::string date_locale = out2.substr(pos2 + 6, out2.find("\r\n", pos2) - pos2 - 6);
    setlocale(LC_ALL, old.c_str());
    EXPECT_EQ(date_en.size(), date_locale.size());      // 结构不变
    EXPECT_EQ(date_en.substr(0, 3), date_locale.substr(0, 3));  // 星期缩写恒英文
    EXPECT_EQ(date_en.substr(0, 3).find_first_of("SunMonTueWedThuFriSat"), 0);  // 首字母属英文星期集
    EXPECT_TRUE(std::all_of(date_en.begin(), date_en.begin() + 3, ::isalpha));
}

/// [正] DumpHead 与 Dump 字节全等（U5：Dump 拆分重构的兼容锁定）
TEST(HttpMessageTest, DumpHeadByteCompatibleWithDump) {
    HttpRequest req;
    req.setMethod(HttpMethod::kPOST);
    req.setMinor(0);
    HttpResponse resp;
    resp.setHeader("X-A", "1");
    resp.addCookie(Cookie{.name = "s", .value = "1"});
    resp.setBody(std::string("payload"));
    EXPECT_EQ(resp.DumpHead(req) + "payload", resp.Dump(req));
}

/// [正] 文件源：DumpHead 输出 CL=fileSize 无 body 字节
TEST(HttpMessageTest, DumpHeadFileSource) {
    std::string tmp = "/tmp/codebus-dumphead-test.bin";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "0123456789";  // 10 字节
    }
    HttpRequest req;
    HttpResponse resp;
    ASSERT_TRUE(resp.setBodyFile(tmp));
    std::string head = resp.DumpHead(req);
    EXPECT_NE(head.find("Content-Length: 10\r\n"), std::string::npos);
    EXPECT_TRUE(head.ends_with("\r\n\r\n"));  // 无 body 字节
    ::unlink(tmp.c_str());
}

/// [界] 不存在路径 → false
TEST(HttpMessageTest, SetBodyFileMissing) {
    HttpResponse resp;
    EXPECT_FALSE(resp.setBodyFile("/nonexistent/codebus/xyz"));
    EXPECT_LT(resp.bodyFileFd(), 0);
}

/// [界] Cookie 属性段注入拒绝（Q39：path/domain/expires/sameSite 全属性检查）
TEST(HttpMessageTest, CookieAttributeInjectionRejected) {
    HttpRequest req;
    HttpResponse resp;
    Cookie bad_path{.name = "s", .value = "v", .path = "/\r\nX-I: 1"};
    Cookie bad_domain{.name = "s2", .value = "v", .domain = "a.com\r\nX-J: 1"};
    Cookie good{.name = "ok2", .value = "1"};
    resp.addCookie(bad_path);
    resp.addCookie(bad_domain);
    resp.addCookie(good);
    std::string out = resp.Dump(req);
    size_t hdr_end = out.find("\r\n\r\n");
    ASSERT_NE(hdr_end, std::string::npos);
    std::string headers = out.substr(0, hdr_end);
    EXPECT_EQ(headers.find("X-I:"), std::string::npos);
    EXPECT_EQ(headers.find("X-J:"), std::string::npos);
    EXPECT_NE(headers.find("Set-Cookie: ok2=1"), std::string::npos);
}
