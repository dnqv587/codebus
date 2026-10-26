//***********************************************
// name      : LoggerManagerTest.cc
// brief     : log 模块单元测试：被测 src/log/Logger.h/.cc（LoggerManager 单例与 LoadConfig 装配）
// attention : LoadConfig 的 name 参数被忽略（Q23：写死读取 sys_config 的
//             "logger" 键）；LoadConfig 对已存在 logger 会追加 appender
//             （不查重）；本目标经 sys_config 全局对象加载临时 toml，目标
//             进程隔离故无跨目标污染
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "Config.h"
#include "log/Logger.h"

#include <fstream>
#include <string>

namespace bus
{
namespace test
{

// ---------------------------------------------------------------------------
// 迁移用例（自旧 LogTest）
// ---------------------------------------------------------------------------

TEST(LoggerManagerTest, LoggerManagerReturnsSameInstance)
{
    Logger* first = GET_LOGGER("unique-manager-test");
    Logger* second = GET_LOGGER("unique-manager-test");
    EXPECT_EQ(first, second);

    EXPECT_EQ(LoggerManager::getInstance(), LoggerManager::getInstance());
}

// ---------------------------------------------------------------------------
// 新增场景（P9）
// ---------------------------------------------------------------------------

/// [正] getLogger 同名复用同一 Logger、新名新建
TEST(LoggerManagerTest, GetLoggerReusesAndCreates)
{
    Logger* alpha = GET_LOGGER("manager-reuse");
    Logger* alpha_again = GET_LOGGER("manager-reuse");
    EXPECT_EQ(alpha, alpha_again);

    Logger* beta = GET_LOGGER("manager-create-" + std::to_string(::getpid()));
    EXPECT_NE(alpha, beta);
    EXPECT_EQ(beta->getName(), "manager-create-" + std::to_string(::getpid()));
}

namespace
{

/// @brief 向 dir 写入 toml 内容并加载进 sys_config
void WriteAndLoad(const std::filesystem::path& dir, const std::string& entries)
{
    const auto toml_path = dir / "log_conf.toml";
    {
        std::ofstream out(toml_path);
        out << entries;
    }
    sys_config.LoadFile(toml_path.string());
}

} // namespace

// TODO(Q23): LoadConfig(name) 的 name 参数被忽略，实际固定读取 sys_config
// 的 "logger" 数组（src/log/Logger.cc:110-113）。
TEST(LoggerManagerTest, LoadConfigAssemblesLoggers)
{
    auto temp = std::make_unique<TempDir>();
    WriteAndLoad(temp->Path(),
        "[[logger]]\n"
        "name = \"cfg-warn\"\n"
        "path = \"" + temp->Path().string() + "\"\n"
        "EnableStdout = false\n"
        "level = \"WARN\"\n"
        "format = \"%m%n\"\n"
        "action = \"SYNC\"\n"
        "FlushInterval = 1\n"
        "FlushLogCount = 1\n"
        "SingleFileSize = \"1MB\"\n"
        "\n"
        "[[logger]]\n"
        "name = \"cfg-error\"\n"
        "path = \"" + temp->Path().string() + "\"\n"
        "EnableStdout = false\n"
        "level = \"ERROR\"\n"
        "action = \"ASYNC\"\n");

    LoggerManager::getInstance()->LoadConfig("ignored-see-Q23");

    Logger* warn_logger = GET_LOGGER("cfg-warn");
    EXPECT_EQ(warn_logger->getLevel(), LogLevel::WARN);

    Logger* error_logger = GET_LOGGER("cfg-error");
    EXPECT_EQ(error_logger->getLevel(), LogLevel::ERROR);

    // FileLogAppender 落盘路径生效
    LOG_WARN(warn_logger) << "loadcfg-payload";
    // Flush 时机由 flushInterval 控制，此处仅验证不崩溃与 logger 可用
    SUCCEED();
}

/// [界] LoadConfig 对已存在 logger 的更新路径（setLevel/setFormatter）
TEST(LoggerManagerTest, LoadConfigUpdatesExistingLogger)
{
    Logger* existing = GET_LOGGER("cfg-update");
    existing->setLevel(LogLevel::TRACE);

    auto temp = std::make_unique<TempDir>();
    WriteAndLoad(temp->Path(),
        "[[logger]]\n"
        "name = \"cfg-update\"\n"
        "path = \"" + temp->Path().string() + "\"\n"
        "EnableStdout = false\n"
        "level = \"FATAL\"\n"
        "action = \"SYNC\"\n");

    LoggerManager::getInstance()->LoadConfig("ignored");

    EXPECT_EQ(existing->getLevel(), LogLevel::FATAL); // 更新为配置级别
}

/// [界] sys_config 无 logger 数组时 LoadConfig 静默返回
TEST(LoggerManagerTest, LoadConfigWithoutArrayIsNoop)
{
    auto temp = std::make_unique<TempDir>();
    const auto toml_path = temp->Path() / "empty.toml";
    {
        std::ofstream out(toml_path);
        out << "# no logger config\n";
    }
    sys_config.LoadFile(toml_path.string());

    LoggerManager::getInstance()->LoadConfig("ignored"); // 不应崩溃
    SUCCEED();
}

} // namespace test
} // namespace bus
