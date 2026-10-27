//***********************************************
// name      : ExceptionTest.cc
// brief     : exception 模块单元测试：被测 src/Exception.hpp（类型层次与 what() 契约）
// attention : LoggerError/AddressError/SocketError 的默认文案存在复制粘贴
//             痕迹（见 Q16），本测试锁定当前字符串，修正文案后需同步更新
//***********************************************
#include <gtest/gtest.h>
#include "Exception.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>

namespace bus {
namespace test {

// ---------------------------------------------------------------------------
// 编译期契约：类型层次
// ---------------------------------------------------------------------------

static_assert(std::is_base_of_v<std::exception, Exception::ConfigUnLoad>, "ConfigUnLoad 应继承 std::exception");
static_assert(std::is_base_of_v<std::logic_error, Exception::DateTimeFormatError>,
              "DateTimeFormatError 应继承 std::logic_error");
static_assert(std::is_base_of_v<std::logic_error, Exception::ConfigError>, "ConfigError 应继承 std::logic_error");
static_assert(std::is_base_of_v<std::logic_error, Exception::LoggerError>, "LoggerError 应继承 std::logic_error");
static_assert(std::is_base_of_v<std::logic_error, Exception::AddressError>, "AddressError 应继承 std::logic_error");
static_assert(std::is_base_of_v<std::logic_error, Exception::SocketError>, "SocketError 应继承 std::logic_error");

// ---------------------------------------------------------------------------
// 运行时断言
// ---------------------------------------------------------------------------

/// [正] 各异常类型默认构造的 what() 文案契约
TEST(ExceptionTest, DefaultMessages) {
    EXPECT_STREQ(Exception::ConfigUnLoad().what(), "config unload");
    EXPECT_STREQ(Exception::DateTimeFormatError().what(), "DateTime Format Error");
    EXPECT_STREQ(Exception::ConfigError().what(), "Config Error");
    // TODO(Q16): 以下三个默认文案为复制粘贴痕迹（LoggerError 并非 ConfigError、
    // "AddressError Error"/"SocketError Error" 语义重复），修正后同步更新断言
    EXPECT_STREQ(Exception::LoggerError().what(), "ConfigError Error");
    EXPECT_STREQ(Exception::AddressError().what(), "AddressError Error");
    EXPECT_STREQ(Exception::SocketError().what(), "SocketError Error");
}

/// [正] 定制文案构造
TEST(ExceptionTest, CustomMessages) {
    EXPECT_STREQ(Exception::DateTimeFormatError("bad format").what(), "bad format");
    EXPECT_STREQ(Exception::ConfigError("load failed").what(), "load failed");
    EXPECT_STREQ(Exception::LoggerError("no appender").what(), "no appender");
    EXPECT_STREQ(Exception::AddressError("bad ip").what(), "bad ip");
    EXPECT_STREQ(Exception::SocketError("bad fd").what(), "bad fd");
}

/// [界] 拷贝与抛出/捕获往返的 what() 稳定；多态捕获可达
TEST(ExceptionTest, ThrowCatchRoundTrip) {
    const std::string thrown = "round trip";

    try {
        throw Exception::AddressError(thrown.c_str());
    } catch (const std::logic_error& err)  // 按基类多态捕获
    {
        EXPECT_STREQ(err.what(), thrown.c_str());

        const std::logic_error copied = err;
        EXPECT_STREQ(copied.what(), thrown.c_str());
    }

    try {
        throw Exception::ConfigUnLoad();
    } catch (const std::exception& err)  // ConfigUnLoad 经 std::exception 多态捕获
    {
        EXPECT_STREQ(err.what(), "config unload");
    }
}

}  // namespace test
}  // namespace bus
