//***********************************************
// name      : LoggerTest.cc
// brief     : log 模块单元测试：被测 src/log/Logger.h/.cc（Logger 类、LOG_* 宏、断言宏）
// attention : 不调用 LoggerManager::LoadConfig（其行为归 LoggerManagerTest）；
//             CHECK_NOTNULL/ASSERT_THROW/LOG_THROW 会向全局 "sys" logger 记
//             FATAL 日志（目标进程隔离，无跨目标污染）；本目标含 D22 禁用用例
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "log/Logger.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

// ---------------------------------------------------------------------------
// 迁移用例（自旧 LogTest）
// ---------------------------------------------------------------------------

TEST(LoggerTest, LevelAndName)
{
    Logger logger("log-test");
    EXPECT_EQ(logger.getName(), "log-test");
    EXPECT_EQ(logger.getLevel(), LogLevel::DEBUG);

    logger.setLevel(LogLevel::ERROR);
    EXPECT_EQ(logger.getLevel(), LogLevel::ERROR);
}

TEST(LoggerTest, StreamMacroWritesToAppender)
{
    Logger logger("stream-test");
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);

    LOG_INFO(logger) << "hello " << 42;

    EXPECT_NE(capture->Contents().find("hello 42"), std::string::npos);
}

TEST(LoggerTest, LevelFiltering)
{
    Logger logger("filter-test", LogLevel::WARN);
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);

    LOG_INFO(logger) << "hidden-message";
    EXPECT_EQ(capture->Contents().find("hidden-message"), std::string::npos);

    LOG_WARN(logger) << "visible-message";
    EXPECT_NE(capture->Contents().find("visible-message"), std::string::npos);
}

TEST(LoggerTest, FormatMacro)
{
    Logger logger("fmt-test");
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);

    LOG_FMT_INFO(logger, "value=%d name=%s", 7, "codebus");
    EXPECT_NE(capture->Contents().find("value=7 name=codebus"), std::string::npos);
}

TEST(LoggerTest, DelAppender)
{
    Logger logger("del-test");
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);
    logger.delAppender(capture);

    LOG_INFO(logger) << "after-remove";
    EXPECT_TRUE(capture->Contents().empty());
}

// 2026-10-19 验证：D22 已不复现（~Logging 经 m_format 格式化后提交，
// src/log/Logging.cc:42-54），本用例由 DISABLED 转为启用的回归用例。
TEST(LoggerTest, FormatterAppliedByLogger)
{
    Logger logger("formatter-apply-test");
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);
    logger.setFormatter("%p|%m%n");

    LOG_INFO(logger) << "with-format";
    EXPECT_NE(capture->Contents().find("INFO|with-format"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 新增场景（P9）
// ---------------------------------------------------------------------------

/// [正] LOG_FMT_* 与流式输出内容等价
TEST(LoggerTest, FmtMacroEquivalentToStream)
{
    Logger fmt_logger("fmt-eq");
    Logger stream_logger("stream-eq");
    auto fmt_capture = std::make_shared<CaptureAppender>();
    auto stream_capture = std::make_shared<CaptureAppender>();
    fmt_logger.addAppender(fmt_capture);
    stream_logger.addAppender(stream_capture);

    LOG_FMT_WARN(fmt_logger, "same-%c-%d", 'x', 3);
    LOG_WARN(stream_logger) << "same-x-3";

    EXPECT_NE(fmt_capture->Contents().find("same-x-3"), std::string::npos);
    EXPECT_NE(stream_capture->Contents().find("same-x-3"), std::string::npos);
}

/// [正] 多 appender 广播：同一条日志两个 appender 都收到
TEST(LoggerTest, MultiAppenderBroadcast)
{
    Logger logger("broadcast-test");
    auto first = std::make_shared<CaptureAppender>();
    auto second = std::make_shared<CaptureAppender>();
    logger.addAppender(first);
    logger.addAppender(second);

    LOG_ERROR(logger) << "broadcast-payload";

    EXPECT_NE(first->Contents().find("broadcast-payload"), std::string::npos);
    EXPECT_NE(second->Contents().find("broadcast-payload"), std::string::npos);
}

/// [界] 删除一个 appender 后，另一个仍正常接收
TEST(LoggerTest, DelOneAppenderKeepsOther)
{
    Logger logger("del-one-test");
    auto removed = std::make_shared<CaptureAppender>();
    auto kept = std::make_shared<CaptureAppender>();
    logger.addAppender(removed);
    logger.addAppender(kept);

    logger.delAppender(removed);

    LOG_INFO(logger) << "after-del-one";
    EXPECT_TRUE(removed->Contents().empty());
    EXPECT_NE(kept->Contents().find("after-del-one"), std::string::npos);
}

// TODO(Q24): 重复 addAppender 同一对象不查重——appender 向量出现重复条目，
// 同一条日志被广播两次（src/log/Logger.cc 的 addAppender）。本用例锁定当前
// 行为；若确认应查重，修复后改为 EXPECT_EQ(count, 1)。
TEST(LoggerTest, DuplicateAddBroadcastsTwice)
{
    Logger logger("dup-add-test");
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);
    logger.addAppender(capture); // 重复添加

    LOG_INFO(logger) << "dup-payload";

    const std::string content = capture->Contents();
    size_t count = 0;
    for (size_t pos = content.find("dup-payload"); pos != std::string::npos;
         pos = content.find("dup-payload", pos + 1))
    {
        ++count;
    }
    EXPECT_EQ(count, 2U); // 现状：重复条目导致双份广播
}

/// [错] CHECK_NOTNULL：空指针记录 FATAL 并原样返回（不抛不断）
TEST(LoggerTest, CheckNotNullLogsAndReturns)
{
    Logger& sys = GET_LOGGER("sys");
    auto capture = std::make_shared<CaptureAppender>();
    sys.addAppender(capture);

    int* null_ptr = nullptr;
    int* result = CHECK_NOTNULL(null_ptr);

    EXPECT_EQ(result, nullptr);
    EXPECT_NE(capture->Contents().find("Must be non NULL"), std::string::npos);

    sys.delAppender(capture); // 恢复全局 sys logger

    int value = 42;
    int* valid = CHECK_NOTNULL(&value);
    EXPECT_EQ(valid, &value);
}

/// [错] ASSERT_THROW：条件为假时抛出指定异常并记录
TEST(LoggerTest, AssertThrowThrowsOnFalse)
{
    EXPECT_THROW(ASSERT_THROW(1 + 1 == 3, std::logic_error("assert-fail")), std::logic_error);

    EXPECT_NO_THROW(ASSERT_THROW(1 + 1 == 2, std::logic_error("never")));
}

/// [错] LOG_THROW：抛出并记录异常
TEST(LoggerTest, LogThrowThrows)
{
    EXPECT_THROW(LOG_THROW(std::runtime_error("log-throw-reason")), std::runtime_error);
}

/// [并] setFormatter 与日志写入并发（RWLock 读写路径）
TEST(LoggerTest, SetFormatterConcurrentWithLogging)
{
    Logger logger("rwlock-storm");
    auto capture = std::make_shared<CaptureAppender>();
    logger.addAppender(capture);

    std::atomic<bool> stop{false};
    std::atomic<int> logged{0};

    std::thread reformatter([&]() {
        while (!stop.load())
        {
            logger.setFormatter("%m%n");
            logger.setFormatter("%p:%m%n");
        }
    });

    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w)
    {
        writers.emplace_back([&]() {
            for (int i = 0; i < 500; ++i)
            {
                LOG_INFO(logger) << "rw-storm";
                logged.fetch_add(1);
            }
        });
    }
    for (auto& thread : writers)
    {
        thread.join();
    }
    stop.store(true);
    reformatter.join();

    EXPECT_EQ(logged.load(), 2000);
    EXPECT_FALSE(capture->Contents().empty());
}

} // namespace test
} // namespace bus
