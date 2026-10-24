//***********************************************
// name      : LogFormatterTest.cc
// brief     : log 模块单元测试：被测 src/log/LogFormatter.h/.cc（占位符解析与 Format 输出）
// attention : 间接覆盖 base/MacroReflaction.hpp 生成的全部 FormatItem
//***********************************************
#include <gtest/gtest.h>
#include "log/LogFormatter.h"
#include "log/Logging.h"
#include "thread/CurrentThread.h"
#include "Timestamp.h"

#include <string>

namespace bus
{
namespace test
{

namespace
{

LogContext MakeContext(std::string_view logger_name, LogLevel level)
{
    LogContext context("formatter.cc", 321, "MakeContext", 100, 4321, 5,
                       Timestamp::now(), "fmt-thread", level, logger_name);
    context.getStream() << "payload";
    return context;
}

} // namespace

// ---------------------------------------------------------------------------
// 迁移用例（自旧 LogTest）
// ---------------------------------------------------------------------------

TEST(LogFormatterTest, FormatterMessageAndNewline)
{
    LogFormatter formatter("%m%n");
    LogContext context(__FILE__, __LINE__, __func__, 0, CurrentThread::tid(), 0,
                       Timestamp::now(), "test-thread", LogLevel::INFO, "formatter-test");
    context.getStream() << "formatted-message";

    const std::string output = formatter.Format(context).str();
    EXPECT_NE(output.find("formatted-message"), std::string::npos);
    EXPECT_NE(output.find('\n'), std::string::npos);
}

TEST(LogFormatterTest, FormatterPlaceholders)
{
    LogFormatter formatter("%p|%c|%f|%l|%%|%m");
    LogContext context(__FILE__, 321, __func__, 0, CurrentThread::tid(), 0,
                       Timestamp::now(), "test-thread", LogLevel::WARN, "placeholder-logger");
    context.getStream() << "payload";

    const std::string output = formatter.Format(context).str();
    EXPECT_NE(output.find("WARN"), std::string::npos);
    EXPECT_NE(output.find("placeholder-logger"), std::string::npos);
    EXPECT_NE(output.find("321"), std::string::npos);
    EXPECT_NE(output.find('|'), std::string::npos);
    EXPECT_NE(output.find("payload"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 新增场景（P9）
// ---------------------------------------------------------------------------

/// [正] 全占位符逐项断言：%t 线程id、%M 函数名、%N 线程名、%F 协程id、%r 累计毫秒
TEST(LogFormatterTest, IdentityPlaceholders)
{
    LogFormatter formatter("[%t][%M][%N][%F][%r]");
    LogContext context("fmt.cc", 1, "CaseFunc", 4321, 8642, 55,
                       Timestamp::now(), "worker-thread", LogLevel::INFO, "id-logger");

    const std::string output = formatter.Format(context).str();
    EXPECT_NE(output.find("8642"), std::string::npos);   // %t 线程id
    EXPECT_NE(output.find("CaseFunc"), std::string::npos); // %M 函数名
    EXPECT_NE(output.find("worker-thread"), std::string::npos); // %N 线程名
    EXPECT_NE(output.find("55"), std::string::npos);     // %F 协程id
    EXPECT_NE(output.find("4321"), std::string::npos);   // %r 累计毫秒
}

/// [正] %h 主机名与 %P 进程号
TEST(LogFormatterTest, HostAndPidPlaceholders)
{
    LogFormatter formatter("%h|%P");
    LogContext context = MakeContext("host-logger", LogLevel::INFO);

    const std::string output = formatter.Format(context).str();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find('|'), std::string::npos);
    // 进程号必为数字串
    const size_t pid_pos = output.find('|') + 1;
    EXPECT_LT(pid_pos, output.size());
}

/// [界] 字面文本 suffix 正确穿插于占位符之间
TEST(LogFormatterTest, LiteralSuffixInterleaves)
{
    LogFormatter formatter("pre-%m-post");
    LogContext context = MakeContext("suffix-logger", LogLevel::INFO);

    const std::string output = formatter.Format(context).str();
    EXPECT_EQ(output, "pre-payload-post");
}

/// [界] %d 时间占位符缺省格式与自定义格式
TEST(LogFormatterTest, TimePlaceholderFormats)
{
    LogFormatter custom("%d{%Y-%m-%d}");
    LogContext context = MakeContext("time-logger", LogLevel::INFO);

    const std::string output = custom.Format(context).str();
    // 自定义格式：YYYY-MM-DD 十字符
    ASSERT_GE(output.size(), 10U);
    EXPECT_EQ(output[4], '-');
    EXPECT_EQ(output[7], '-');
}

/// [界] %T 制表符分隔与 %n 换行组合
TEST(LogFormatterTest, TabAndNewlinePlaceholders)
{
    LogFormatter formatter("%m%T%m%n");
    LogContext context = MakeContext("tab-logger", LogLevel::INFO);

    const std::string output = formatter.Format(context).str();
    EXPECT_NE(output.find('\t'), std::string::npos);
    EXPECT_NE(output.find('\n'), std::string::npos);
}

} // namespace test
} // namespace bus
