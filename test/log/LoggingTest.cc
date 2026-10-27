//***********************************************
// name      : LoggingTest.cc
// brief     : log 模块单元测试：被测 src/log/Logging.h/.cc（流式 RAII 提交、LogContext、LogLevel）
// attention : Logging 析构时格式化并提交（RAII 契约）；析构会在 logger 无
//             formatter 时自动安装默认模板
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "log/Logging.h"
#include "log/Logger.h"

#include <memory>

namespace bus {
namespace test {

/// [正] RAII 提交契约：构造后未析构前 appender 无内容，析构后才提交
TEST(LoggingTest, DestructorCommitsToAppender) {
    Logger logger("raii-commit-test");
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);

    auto pending = std::make_unique<Logging>(&logger, __FILE__, __LINE__, __FUNCTION__, LogLevel::INFO);
    pending->getStream() << "before-destructor";

    EXPECT_TRUE(capture->Contents().empty());  // 未析构：不提交

    pending.reset();  // 析构：格式化并提交

    EXPECT_NE(capture->Contents().find("before-destructor"), std::string::npos);
}

/// [正] LogFormat 写入消息流（与 getStream 等价路径）
TEST(LoggingTest, LogFormatWritesToStream) {
    Logger logger("logformat-test");
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);

    {
        Logging log(&logger, __FILE__, __LINE__, __FUNCTION__, LogLevel::INFO);
        log.LogFormat("fmt-%d-str-%s", 7, "codebus");
    }

    EXPECT_NE(capture->Contents().find("fmt-7-str-codebus"), std::string::npos);
}

/// [正] LogContext 字段填充与读取
TEST(LoggingTest, LogContextFields) {
    const Timestamp now = Timestamp::now();
    LogContext context("demo.cc", 42, "DemoFunc", 1234, 999, 7, now, "ctx-thread", LogLevel::WARN, "ctx-logger");
    context.getStream() << "payload";

    EXPECT_STREQ(context.getSourceFile(), "demo.cc");
    EXPECT_EQ(context.getLine(), 42);
    EXPECT_EQ(context.getFuncName(), "DemoFunc");
    EXPECT_EQ(context.getElapse(), 1234);
    EXPECT_EQ(context.getThreadId(), 999);
    EXPECT_EQ(context.getFiberId(), 7U);
    EXPECT_EQ(context.getTime().getMicroSecond(), now.getMicroSecond());
    EXPECT_EQ(context.getThreadName(), "ctx-thread");
    EXPECT_EQ(context.getLoggerName(), "ctx-logger");
    EXPECT_EQ(context.getLogLevel(), LogLevel::WARN);
    EXPECT_NE(context.getStream().str().find("payload"), std::string::npos);
}

/// [界] LogLevel 枚举序关系与个数
TEST(LoggingTest, LogLevelOrdering) {
    EXPECT_LT(LogLevel::TRACE, LogLevel::DEBUG);
    EXPECT_LT(LogLevel::DEBUG, LogLevel::INFO);
    EXPECT_LT(LogLevel::INFO, LogLevel::WARN);
    EXPECT_LT(LogLevel::WARN, LogLevel::ERROR);
    EXPECT_LT(LogLevel::ERROR, LogLevel::FATAL);
    EXPECT_LT(LogLevel::FATAL, LogLevel::NUM_LOG_LEVELS);
    EXPECT_EQ(static_cast<int>(LogLevel::NUM_LOG_LEVELS), 6);
}

}  // namespace test
}  // namespace bus
