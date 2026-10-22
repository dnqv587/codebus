//***********************************************
// name      : ConfigTest.cc
// brief     : Config 模块单元测试：被测 src/Config.h/.cc（TOML 加载、类型访问、路径查询）
// attention : 全部使用内嵌 TOML 或临时文件，不依赖工作目录（sys_config 的
//             LoadConfig 联动归 LoggerManagerTest）；本目标含 D8 禁用用例
//***********************************************
#include <gtest/gtest.h>
#include "Config.h"
#include "TestHelper.h"
#include "exception/Exception.hpp"

#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace bus
{
namespace test
{

namespace
{

constexpr std::string_view kToml = R"(
[string_test]
name = "hello world"
message = 'escaped "string"'

[int_test]
count = 42
negative = -100

[float_test]
pi = 3.14159

[bool_test]
enabled = true
disabled = false

[date_test]
created = 2025-01-19

[time_test]
alarm = 14:30:00.123

[datetime_test]
timestamp = 2025-01-19T14:30:00

[array_test]
numbers = [1, 2, 3]
names = ["alice", "bob"]
empty = []

[table_test]
key = "value"
number = 123

[[array_table_test.items]]
id = 1
name = "item1"

[[array_table_test.items]]
id = 2
name = "item2"

[nested_test]
server = { host = "localhost", port = 8080 }
)";

} // namespace

TEST(ConfigTest, LoadStringAndScalarTypes)
{
    Config config;
    ASSERT_NO_THROW(config.Load(kToml));
    EXPECT_TRUE(config.isLoaded());

    auto name = config.get("string_test.name");
    ASSERT_NE(name, nullptr);
    ASSERT_TRUE(name->isString());
    ASSERT_TRUE(name->asString().has_value());
    EXPECT_EQ(name->asString().value(), "hello world");

    auto count = config.get("int_test.count");
    ASSERT_TRUE(count->isInteger());
    EXPECT_EQ(count->asInteger().value(), 42);

    auto negative = config.get("int_test.negative");
    EXPECT_EQ(negative->asInteger().value(), -100);

    auto pi = config.get("float_test.pi");
    ASSERT_TRUE(pi->isFloat());
    EXPECT_DOUBLE_EQ(pi->asFloat().value(), 3.14159);

    EXPECT_TRUE(config.get("bool_test.enabled")->asBoolean().value());
    EXPECT_FALSE(config.get("bool_test.disabled")->asBoolean().value());
}

TEST(ConfigTest, DateAndTimeTypes)
{
    Config config;
    config.Load(kToml);

    auto date = config.get("date_test.created")->asDate();
    ASSERT_TRUE(date.has_value());
    EXPECT_EQ(static_cast<int>(date->year()), 2025);
    EXPECT_EQ(static_cast<unsigned>(date->month()), 1U);
    EXPECT_EQ(static_cast<unsigned>(date->day()), 19U);

    auto time = config.get("time_test.alarm")->asTime();
    ASSERT_TRUE(time.has_value());
    EXPECT_EQ(time->hours().count(), 14);
    EXPECT_EQ(time->minutes().count(), 30);
    EXPECT_EQ(time->seconds().count(), 0);

    auto datetime = config.get("datetime_test.timestamp")->asDateTime();
    ASSERT_TRUE(datetime.has_value());
    EXPECT_GT(datetime->time_since_epoch().count(), 0);
}

TEST(ConfigTest, ArraysAndTables)
{
    Config config;
    config.Load(kToml);

    auto numbers = config.get("array_test.numbers")->asArray();
    ASSERT_TRUE(numbers.has_value());
    ASSERT_EQ(numbers->size(), 3U);
    EXPECT_EQ((*numbers)[0]->asInteger().value(), 1);
    EXPECT_EQ((*numbers)[2]->asInteger().value(), 3);

    auto names = config.get("array_test.names")->asArray();
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ((*names)[0]->asString().value(), "alice");

    auto empty = config.get("array_test.empty")->asArray();
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());

    auto table = config.get("table_test")->asTable();
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(table->at("key")->asString().value(), "value");
    EXPECT_EQ(table->at("number")->asInteger().value(), 123);
}

TEST(ConfigTest, ArrayTableElementsReadableAsTables)
{
    Config config;
    config.Load(kToml);

    // asArrayTable 尚未实现（D8），但可通过 asArray + asTable 读取
    auto items = config.get("array_table_test.items")->asArray();
    ASSERT_TRUE(items.has_value());
    ASSERT_EQ(items->size(), 2U);
    EXPECT_EQ((*items)[0]->asTable()->at("id")->asInteger().value(), 1);
    EXPECT_EQ((*items)[1]->asTable()->at("name")->asString().value(), "item2");
}

TEST(ConfigTest, NestedPath)
{
    Config config;
    config.Load(kToml);

    EXPECT_EQ(config.get("nested_test.server.host")->asString().value(), "localhost");
    EXPECT_EQ(config.get("nested_test.server.port")->asInteger().value(), 8080);
}

TEST(ConfigTest, MissingKeyReturnsNullValue)
{
    Config config;
    config.Load(kToml);

    auto missing = config.get("nonexistent.key");
    ASSERT_NE(missing, nullptr);
    EXPECT_FALSE(missing->isString());
    EXPECT_FALSE(missing->isInteger());
    EXPECT_FALSE(missing->isArray());
    EXPECT_FALSE(missing->asString().has_value());
    EXPECT_FALSE(missing->asInteger().has_value());
}

TEST(ConfigTest, TypeMismatchReturnsNullopt)
{
    Config config;
    config.Load(kToml);

    EXPECT_FALSE(config.get("int_test.count")->asString().has_value());
    EXPECT_FALSE(config.get("string_test.name")->asInteger().has_value());
}

TEST(ConfigTest, GetBeforeLoadThrows)
{
    Config config;
    EXPECT_THROW(config.get("any.key"), Exception::ConfigUnLoad);
}

TEST(ConfigTest, InvalidTomlThrows)
{
    Config config;
    EXPECT_THROW(config.Load("invalid = ["), Exception::ConfigError);
    EXPECT_FALSE(config.isLoaded());
}

TEST(ConfigTest, LoadFile)
{
    TempDir temp;
    const auto file = temp.Path() / "test.toml";
    {
        std::ofstream output(file);
        ASSERT_TRUE(output.is_open());
        output << kToml;
    }

    Config config;
    ASSERT_NO_THROW(config.LoadFile(file));
    EXPECT_TRUE(config.isLoaded());
    EXPECT_EQ(config.get("string_test.name")->asString().value(), "hello world");
}

TEST(ConfigTest, ReloadReplacesValues)
{
    Config config;
    config.Load("[app]\nname = \"first\"\n");
    EXPECT_EQ(config.get("app.name")->asString().value(), "first");

    config.Load("[app]\nname = \"second\"\n");
    EXPECT_EQ(config.get("app.name")->asString().value(), "second");
}

/// @attention 仅覆盖标量路径（asInteger 数据自持有）；asTable/asArray 产出的
///            Value 经 Q29 的 move 浅拷贝与原解析树共享指针，Config 析构后访问
///            即悬垂——复合类型的生命周期不适用本用例结论
TEST(ConfigTest, ValueOutlivesConfig)
{
    Value::ptr value;
    {
        Config config;
        config.Load(kToml);
        value = config.get("int_test.count");
    }
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->asInteger().value(), 42);
}

// TODO(D8): Value4Toml::asArrayTable() 直接返回 nullopt（src/Config.cc:448-462），
// 恢复被注释的实现后启用本用例。
TEST(ConfigTest, DISABLED_ArrayTable)
{
    Config config;
    config.Load(kToml);

    auto items = config.get("array_table_test.items")->asArrayTable();
    ASSERT_TRUE(items.has_value());
    ASSERT_EQ(items->size(), 2U);
}

// ---------------------------------------------------------------------------
// 新增场景（P10）
// ---------------------------------------------------------------------------

/// [并] 8 线程并发 get 同一与不同 key，取值一致（shared_mutex 读路径）
TEST(ConfigTest, ConcurrentGetIsConsistent)
{
    Config config;
    config.Load(kToml);

    constexpr int kThreads = 8;
    constexpr int kReadsPerThread = 2000;
    std::atomic<int> mismatches{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t)
    {
        readers.emplace_back([&, t]() {
            for (int i = 0; i < kReadsPerThread; ++i)
            {
                // 同一 key
                auto name = config.get("string_test.name");
                if (!name->asString().has_value() || name->asString().value() != "hello world")
                {
                    mismatches.fetch_add(1);
                }
                // 不同 key（按线程错开）
                const std::string key = "array_test.numbers";
                if (t % 2 == 0)
                {
                    auto number = config.get(key);
                    if (!number->asArray().has_value())
                    {
                        mismatches.fetch_add(1);
                    }
                }
                else
                {
                    auto count = config.get("int_test.count");
                    if (!count->asInteger().has_value() || count->asInteger().value() != 42)
                    {
                        mismatches.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto& thread : readers)
    {
        thread.join();
    }

    EXPECT_EQ(mismatches.load(), 0);
}

/// [错] LoadFile 不存在的文件抛 ConfigError
TEST(ConfigTest, LoadMissingFileThrows)
{
    TempDir temp;
    Config config;
    EXPECT_THROW(config.LoadFile(temp.Path() / "no_such_file.toml"), Exception::ConfigError);
    EXPECT_FALSE(config.isLoaded());
}

/// [界] 空文档与仅注释文档：加载成功，任意 key 返回 NullValue
TEST(ConfigTest, EmptyDocumentLoads)
{
    Config empty;
    ASSERT_NO_THROW(empty.Load(""));
    EXPECT_TRUE(empty.isLoaded());
    auto value = empty.get("any.key");
    ASSERT_NE(value, nullptr);
    EXPECT_FALSE(value->isString());
    EXPECT_FALSE(value->asString().has_value());

    Config comment_only;
    ASSERT_NO_THROW(comment_only.Load("# just a comment\n"));
    EXPECT_TRUE(comment_only.isLoaded());
    EXPECT_FALSE(comment_only.get("x.y")->asInteger().has_value());
}

/// [正] SYS_CONF 宏与 sys_config 全局对象冒烟（启动时无 ./sys.toml 则未加载）
TEST(ConfigTest, SysConfigMacroSmoke)
{
    if (!sys_config.isLoaded())
    {
        SUCCEED() << "无 ./sys.toml 环境，get 应抛 ConfigUnLoad";
        EXPECT_THROW(SYS_CONF("any.key"), Exception::ConfigUnLoad);
        return;
    }
    // 已加载环境（例如其他用例 LoadFile 过）：宏等价于 sys_config.get
    auto value = SYS_CONF("string_test.name");
    EXPECT_EQ(value->asString().value_or("hello world"), "hello world");
}

} // namespace test
} // namespace bus
