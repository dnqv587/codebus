//***********************************************
// name      : LogAppenderTest.cc
// brief     : log 模块单元测试：被测 src/log/LogAppender.h/.cc（文件落地/滚动/Flush/Stdout）
// attention : 异步 appender 用例依赖后台线程排空（stop 后断言）
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "log/LogAppender.h"
#include "log/Logging.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace bus {
namespace test {

namespace {

std::string ReadSingleLogFile(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            std::ifstream input(entry.path());
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
    }
    return {};
}

size_t CountLogFiles(const std::filesystem::path& directory) {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

size_t CountLines(const std::string& text) {
    size_t lines = 0;
    for (char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    return lines;
}

}  // namespace

// ---------------------------------------------------------------------------
// 迁移用例（自旧 LogTest）
// ---------------------------------------------------------------------------

TEST(LogAppenderTest, FileAppenderWritesFile) {
    TempDir temp;
    {
        FileLogAppender appender("filelog", temp.Path().string(), AppenderAction::SYNC, 3, 1, 4_MB);
        appender.append("file-line-1\n");
        appender.append("file-line-2\n");
    }

    const std::string content = ReadSingleLogFile(temp.Path());
    EXPECT_NE(content.find("file-line-1"), std::string::npos);
    EXPECT_NE(content.find("file-line-2"), std::string::npos);
}

/// [正] 异步 appender 基础落盘：单条日志经后台线程写入文件（stop 排空后可见）
TEST(LogAppenderTest, AsyncFileAppenderWritesFile) {
    TempDir temp;
    {
        AsynAppendFile appender("asynclog", temp.Path().string(), 4_MB, 1, 1);
        appender.Append("async-line\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        appender.stop();
    }

    const std::string content = ReadSingleLogFile(temp.Path());
    EXPECT_NE(content.find("async-line"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 新增场景（P9）
// ---------------------------------------------------------------------------

/// [命] 同步 Flush 后内容立即可见（无需等待析构/滚动）
TEST(LogAppenderTest, SyncFlushMakesContentVisible) {
    TempDir temp;
    {
        SyncAppendFile appender("syncflush", temp.Path().string(), 4_MB, 3, 1);
        appender.Append("flush-me\n");
        appender.Flush();

        const std::string content = ReadSingleLogFile(temp.Path());
        EXPECT_NE(content.find("flush-me"), std::string::npos);
    }
}

/// [正] 超过 rollSize 滚动出多个文件且旧文件保留（小 rollSize 场景）
TEST(LogAppenderTest, RollFileOnSizeExceeded) {
    TempDir temp;
    {
        FileLogAppender appender("rolllog", temp.Path().string(), AppenderAction::SYNC, 3, 1, 1_KB);
        for (int i = 0; i < 64; ++i) {
            appender.append("roll-line-padding-0123456789-0123456789\n");
        }
    }

    EXPECT_GE(CountLogFiles(temp.Path()), 2U);
}

/// [正] 异步停止排空队列：200 条日志全部落盘
TEST(LogAppenderTest, AsyncDrainsAllLinesAfterStop) {
    TempDir temp;
    constexpr int kLines = 200;
    {
        AsynAppendFile appender("asynclog", temp.Path().string(), 4_MB, 1, 1);
        appender.start();
        for (int i = 0; i < kLines; ++i) {
            appender.Append("async-line-" + std::to_string(i) + "\n");
        }
        appender.stop();  // 应排空队列后退出
    }

    const std::string content = ReadSingleLogFile(temp.Path());
    EXPECT_EQ(CountLines(content), static_cast<size_t>(kLines)) << "异步停止应排空全部日志";
}

/// [并] 8 线程并发向同步 appender 追加，落盘行数守恒
TEST(LogAppenderTest, ConcurrentAppendConservation) {
    TempDir temp;
    constexpr int kThreads = 8;
    constexpr int kLinesPerThread = 200;
    constexpr int kTotal = kThreads * kLinesPerThread;
    {
        FileLogAppender appender("concurrent", temp.Path().string(), AppenderAction::SYNC, 3, 1024, 4_MB);

        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&appender]() {
                for (int i = 0; i < kLinesPerThread; ++i) {
                    appender.append("c-line\n");
                }
            });
        }
        for (auto& thread : writers) {
            thread.join();
        }
    }

    size_t total_lines = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temp.Path())) {
        if (entry.is_regular_file()) {
            std::ifstream input(entry.path());
            const std::string text{std::istreambuf_iterator<char>(input), {}};
            total_lines += CountLines(text);
        }
    }
    EXPECT_EQ(total_lines, static_cast<size_t>(kTotal));
}

/// [界] StdoutLogAppender 冒烟（不断言 stdout 内容，只验证不崩溃）
TEST(LogAppenderTest, StdoutAppenderSmoke) {
    StdoutLogAppender appender;
    appender.append("stdout-smoke\n");
    appender.append("stdout-smoke-with-level\n", LogLevel::INFO);
    SUCCEED();
}

}  // namespace test
}  // namespace bus
