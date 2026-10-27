//***********************************************
// name      : HttpRequestParserTest.cc
// brief     : HttpRequestParser 全状态机测试（纯函数层，无网络）
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "net/http/HttpRequestParser.h"

using namespace bus;
using namespace bus::http;

namespace
{

/// @brief 喂入整段并断言 Complete
ParseResult FeedComplete(HttpRequestParser& p, std::string_view raw)
{
    ParseResult r = p.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kComplete);
    return r;
}

} // namespace

/// [正] 基本 GET 一次喂入
TEST(HttpRequestParserTest, SimpleGet)
{
    HttpRequestParser parser;
    std::string raw = "GET /path?k=v HTTP/1.1\r\nHost: codebus\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kComplete);
    EXPECT_EQ(r.consumed, raw.size());
    EXPECT_EQ(r.error, HttpParseError::kNone);

    HttpRequest req = parser.TakeRequest();
    EXPECT_EQ(req.getMethod(), HttpMethod::kGET);
    EXPECT_EQ(req.getHttpMajor(), 1);
    EXPECT_EQ(req.getHttpMinor(), 1);
    EXPECT_EQ(req.getVersion(), "HTTP/1.1");
    EXPECT_EQ(req.getRawUrl(), "/path?k=v");
    EXPECT_EQ(req.getPath(), "/path");
    EXPECT_EQ(req.getQuery(), "k=v");
    ASSERT_TRUE(req.getParam("k").has_value());
    EXPECT_EQ(*req.getParam("k"), "v");
    ASSERT_TRUE(req.getHeader("HOST").has_value());
    EXPECT_EQ(*req.getHeader("host"), "codebus");
    EXPECT_TRUE(req.isKeepAlive());
    EXPECT_FALSE(req.isChunked());
    EXPECT_TRUE(req.getBody().empty());
}

/// [正] 逐字节喂入（分片拼接：URL/header 任意断点）
TEST(HttpRequestParserTest, ByteByByte)
{
    HttpRequestParser parser;
    std::string raw = "GET /a%20b?q=1 HTTP/1.1\r\nContent-Type: text/html\r\nHost: h\r\n\r\n";
    for (size_t i = 0; i + 1 < raw.size(); ++i)
    {
        ParseResult r = parser.Execute(raw.data() + i, 1);
        ASSERT_EQ(r.event, ParseEvent::kNeedMore) << " at byte " << i;
    }
    ParseResult r = parser.Execute(raw.data() + raw.size() - 1, 1);
    EXPECT_EQ(r.event, ParseEvent::kComplete);
    EXPECT_EQ(r.consumed, 1u);

    HttpRequest req = parser.TakeRequest();
    EXPECT_EQ(req.getPath(), "/a b");            // %20 解码，跨分片拼接
    ASSERT_TRUE(req.getHeader("content-type").has_value());
    EXPECT_EQ(*req.getHeader("content-type"), "text/html");
    ASSERT_TRUE(req.getParam("q").has_value());
    EXPECT_EQ(*req.getParam("q"), "1");
}

/// [正] pipeline：两请求拼接一次喂入，consumed 精确停在边界
TEST(HttpRequestParserTest, PipelinedTwoRequests)
{
    HttpRequestParser parser;
    std::string first = "GET /one HTTP/1.1\r\nHost: h\r\n\r\n";
    std::string second = "GET /two HTTP/1.1\r\nHost: h\r\n\r\n";
    std::string both = first + second;

    ParseResult r1 = parser.Execute(both.data(), both.size());
    ASSERT_EQ(r1.event, ParseEvent::kComplete);
    EXPECT_EQ(r1.consumed, first.size());        // 边界精确（pause 语义）
    HttpRequest req1 = parser.TakeRequest();
    EXPECT_EQ(req1.getPath(), "/one");

    ParseResult r2 = parser.Execute(both.data() + r1.consumed, both.size() - r1.consumed);
    ASSERT_EQ(r2.event, ParseEvent::kComplete);
    EXPECT_EQ(r2.consumed, second.size());
    EXPECT_EQ(parser.TakeRequest().getPath(), "/two");
    EXPECT_EQ(parser.parsedCount(), 2u);
    EXPECT_TRUE(parser.isIdle());
}

/// [正] POST 定长 body
TEST(HttpRequestParserTest, PostBody)
{
    HttpRequestParser parser;
    std::string raw = "POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";
    ParseResult r = FeedComplete(parser, raw);
    EXPECT_EQ(r.consumed, raw.size());
    HttpRequest req = parser.TakeRequest();
    EXPECT_EQ(req.getMethod(), HttpMethod::kPOST);
    EXPECT_EQ(req.getBody(), "hello");
    EXPECT_EQ(req.getContentLength(), 5u);
}

/// [正] chunked body（on_chunk 回调不需特判，body 字节流经同一 on_body 累积）
TEST(HttpRequestParserTest, ChunkedBody)
{
    HttpRequestParser parser;
    std::string raw =
        "POST /up HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n3\r\n wo\r\n2\r\nrl\r\n1\r\nd\r\n0\r\n\r\n";
    ParseResult r = FeedComplete(parser, raw);
    HttpRequest req = parser.TakeRequest();
    EXPECT_TRUE(req.isChunked());
    EXPECT_EQ(req.getBody(), "hello world");
}

/// [正] 无 Content-Length 的 GET：body 空且完成
TEST(HttpRequestParserTest, NoContentLengthGet)
{
    HttpRequestParser parser;
    FeedComplete(parser, "GET / HTTP/1.0\r\nHost: h\r\n\r\n");
    HttpRequest req = parser.TakeRequest();
    EXPECT_TRUE(req.getBody().empty());
    EXPECT_FALSE(req.isKeepAlive());             // HTTP/1.0 默认 close
}

/// [正] Expect: 100-continue 回调恰触发一次
TEST(HttpRequestParserTest, ExpectContinueCallback)
{
    HttpRequestParser parser;
    int fired = 0;
    parser.setOnExpectContinue([&] { ++fired; });
    FeedComplete(parser,
                 "POST /big HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                 "Content-Length: 2\r\n\r\nok");
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(parser.TakeRequest().isExpectContinue());
}

/// [界] CL 前置超限：零字节即拒（413 路径）
TEST(HttpRequestParserTest, BodyTooLargeClRejected)
{
    HttpRequestParser parser(8, 1024);           // maxBody=8
    std::string raw = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\naaaaaaaaa";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBodyTooLarge);
    EXPECT_LT(r.consumed, raw.size());           // 未吞 body
    EXPECT_TRUE(parser.isIdle());                // 错误后整体复位
}

/// [界] chunked 累计超限（on_body 路径）
TEST(HttpRequestParserTest, ChunkedBodyTooLarge)
{
    HttpRequestParser parser(8, 1024);
    std::string raw =
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBodyTooLarge);
}

/// [界] 头超 80KB 内建上限 → 431
TEST(HttpRequestParserTest, HeaderTooLarge)
{
    HttpRequestParser parser;
    std::string raw = "GET / HTTP/1.1\r\n";
    raw += "X-Big: " + std::string(90 * 1024, 'a') + "\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kHeaderTooLarge);
}

/// [界] 坏版本（major 双位数非法；注：HTTP/9.9 会被解析为 major=9 并非错误）
TEST(HttpRequestParserTest, MalformedBadVersion)
{
    HttpRequestParser parser;
    std::string raw = "GET / HTTP/12.3\r\nHost: h\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBadRequest);
}

/// [界] 坏方法
TEST(HttpRequestParserTest, MalformedBadMethod)
{
    HttpRequestParser parser;
    ParseResult r = parser.Execute("FOO / HTTP/1.1\r\n\r\n", 20);
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBadRequest);
}

/// [界] 双 Content-Length 不同值（严格模式拒绝）
TEST(HttpRequestParserTest, MalformedDoubleCl)
{
    HttpRequestParser parser;
    std::string raw = "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\naa";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBadRequest);
}

/// [界] CL 与 Transfer-Encoding 并存（smuggling 防护）
TEST(HttpRequestParserTest, MalformedClAndChunked)
{
    HttpRequestParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBadRequest);
}

/// [界] 坏 chunk size
TEST(HttpRequestParserTest, MalformedBadChunkSize)
{
    HttpRequestParser parser;
    std::string raw = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBadRequest);
}

/// [正] WebSocket 升级头 → kUpgrade（consumed 停在头部结束）
TEST(HttpRequestParserTest, UpgradeDetected)
{
    HttpRequestParser parser;
    std::string raw =
        "GET /chat HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: k\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kUpgrade);
    EXPECT_EQ(r.consumed, raw.size());
}

/// [正] CONNECT → kUpgrade（V1 服务端 501 路径）
TEST(HttpRequestParserTest, ConnectRejected)
{
    HttpRequestParser parser;
    std::string raw = "CONNECT example.com:80 HTTP/1.1\r\nHost: h\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kUpgrade);
}

/// [界] URL 超限 → 414
TEST(HttpRequestParserTest, UrlTooLong)
{
    HttpRequestParser parser(8_MB, 64);          // maxUrl=64
    std::string raw = "GET /" + std::string(128, 'x') + " HTTP/1.1\r\nHost: h\r\n\r\n";
    ParseResult r = parser.Execute(raw.data(), raw.size());
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kUrlTooLong);
}

/// [界] path 含 %00 → 400（防路径截断）
TEST(HttpRequestParserTest, PathNulRejected)
{
    HttpRequestParser parser;
    ParseResult r = parser.Execute("GET /a%00b HTTP/1.1\r\nHost: h\r\n\r\n", 36);
    EXPECT_EQ(r.event, ParseEvent::kError);
    EXPECT_EQ(r.error, HttpParseError::kBadRequest);
}

/// [正] MessageBegin 回调计数 = 完成请求数（pipeline 须循环喂：pause 后同次 Execute 不续解析）
TEST(HttpRequestParserTest, MessageBeginCallback)
{
    HttpRequestParser parser;
    int begins = 0;
    parser.setOnMessageBegin([&] { ++begins; });
    std::string one = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
    parser.Execute(one.data(), one.size());
    std::string two = one + one;
    size_t off = 0;
    while (off < two.size())
    {
        ParseResult r = parser.Execute(two.data() + off, two.size() - off);
        ASSERT_NE(r.event, ParseEvent::kNeedMore);
        ASSERT_GT(r.consumed, 0u);
        off += r.consumed;
    }
    EXPECT_EQ(begins, 3);
    EXPECT_EQ(parser.parsedCount(), 3u);
}

/// [正] 同一 parser 复用 100 轮（keep-alive 语义）
TEST(HttpRequestParserTest, ReuseHundredTimes)
{
    HttpRequestParser parser;
    std::string raw = "GET /loop HTTP/1.1\r\nHost: h\r\n\r\n";
    for (int i = 0; i < 100; ++i)
    {
        ParseResult r = parser.Execute(raw.data(), raw.size());
        ASSERT_EQ(r.event, ParseEvent::kComplete) << " round " << i;
        ASSERT_EQ(r.consumed, raw.size());
        HttpRequest req = parser.TakeRequest();
        ASSERT_EQ(req.getPath(), "/loop") << " round " << i;
    }
    EXPECT_EQ(parser.parsedCount(), 100u);
}
