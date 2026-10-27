//***********************************************
// name      : HttpUtilTest.cc
// brief     : HttpUtil 纯函数工具测试（URL 编解码/query 解析/EqualCI）
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "net/http/HttpUtil.h"

using namespace bus::http;

/// [正] %XX 基本解码
TEST(HttpUtilTest, DecodeBasics)
{
    EXPECT_EQ(UrlDecode("%48%65%6C%6C%6F", false), "Hello");
    EXPECT_EQ(UrlDecode("%E4%B8%AD", false), "\xE4\xB8\xAD");   // UTF-8 中文字节
    EXPECT_EQ(UrlDecode("%20", false), " ");
    EXPECT_EQ(UrlDecode("plain", false), "plain");              // 无转义原样
}

/// [界] '+' 语义：仅 plusAsSpace=true 时按空格（path 中 '+' 为字面量）
TEST(HttpUtilTest, DecodePlusSemantics)
{
    EXPECT_EQ(UrlDecode("a+b", true), "a b");
    EXPECT_EQ(UrlDecode("a+b", false), "a+b");
}

/// [界] 非法 % 序列宽容保留字面（不 400）
TEST(HttpUtilTest, DecodeInvalidEscape)
{
    EXPECT_EQ(UrlDecode("%G1", false), "%G1");    // 后随非 hex
    EXPECT_EQ(UrlDecode("abc%", false), "abc%");  // 结尾截断
    EXPECT_EQ(UrlDecode("%1", false), "%1");      // 一位截断
    EXPECT_EQ(UrlDecode("100%", false), "100%");
}

/// [界] %00 解码为 NUL 字节（std::string 可含 NUL；path 层由 parser 拒绝）
TEST(HttpUtilTest, DecodeNulByte)
{
    std::string s = UrlDecode("a%00b", false);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s[1], '\0');
}

/// [正] 编码：unreserved 原样，其余转 %XX，空格转 %20（非 '+'）
TEST(HttpUtilTest, EncodeBasics)
{
    EXPECT_EQ(UrlEncode("Hello-._~"), "Hello-._~");
    EXPECT_EQ(UrlEncode("a b"), "a%20b");
    EXPECT_EQ(UrlEncode("\xE4\xB8\xAD"), "%E4%B8%AD");
    EXPECT_EQ(UrlEncode("a&b=c?d"), "a%26b%3Dc%3Fd");
}

/// [正] 编码解码往返（plusAsSpace=false 保 '+' 字面量）
TEST(HttpUtilTest, EncodeDecodeRoundTrip)
{
    std::string raw = "a+b=c&d?e#f %g\xE4\xB8\xAD";
    EXPECT_EQ(UrlDecode(UrlEncode(raw), false), raw);
}

/// [正] query 解析：基本对 + %XX/'+' 解码
TEST(HttpUtilTest, ParseQueryBasics)
{
    std::vector<std::pair<std::string, std::string>> out;
    ASSERT_TRUE(ParseQuery("a=1&b=2", out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, "a");
    EXPECT_EQ(out[0].second, "1");
    EXPECT_EQ(out[1].first, "b");
    EXPECT_EQ(out[1].second, "2");

    ASSERT_TRUE(ParseQuery("k=%2B", out));    // %2B 解码为 '+'
    EXPECT_EQ(out[0].second, "+");
    ASSERT_TRUE(ParseQuery("a=1+2", out));    // query 内 '+' 按空格
    EXPECT_EQ(out[0].second, "1 2");
}

/// [界] 无 '=' 段：value 为空（Java getParameter 空值语义）
TEST(HttpUtilTest, ParseQueryNoEqual)
{
    std::vector<std::pair<std::string, std::string>> out;
    ASSERT_TRUE(ParseQuery("flag", out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, "flag");
    EXPECT_TRUE(out[0].second.empty());
}

/// [界] 空段忽略；全空段/空串返回 false
TEST(HttpUtilTest, ParseQueryEmptySeg)
{
    std::vector<std::pair<std::string, std::string>> out;
    ASSERT_TRUE(ParseQuery("a=1&&b=2", out));
    EXPECT_EQ(out.size(), 2u);

    EXPECT_FALSE(ParseQuery("&&", out));
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(ParseQuery("", out));
}

/// [正] ASCII 大小写不敏感比较
TEST(HttpUtilTest, EqualCI)
{
    EXPECT_TRUE(EqualCI("Content-Type", "content-type"));
    EXPECT_TRUE(EqualCI("CONTENT-TYPE", "content-type"));
    EXPECT_TRUE(EqualCI("", ""));
    EXPECT_FALSE(EqualCI("Content-Type", "content-length"));
    EXPECT_FALSE(EqualCI("abc", "abcd"));   // 长度不等
    EXPECT_FALSE(EqualCI("a=c", "a;d"));    // 非字母字符精确比较
    EXPECT_TRUE(EqualCI("123", "123"));
}
