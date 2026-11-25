//***********************************************
// name      : ConfigTest.cc
// brief     : Config 模块单元测试：被测 src/Config.h/.cc（TOML 加载、类型访问、路径查询）
// attention : 全部使用内嵌 TOML 或临时文件，不依赖工作目录（sys_config 的
//             LoadConfig 联动归 LoggerManagerTest）；本目标含 D8 禁用用例
//***********************************************
#include <gtest/gtest.h>
#include "Config.h"
#include "TestHelper.h"
#include "Exception.hpp"

#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace bus {
namespace test {

namespace {

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

}  // namespace

TEST(ConfigTest, LoadStringAndScalarTypes) {
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

TEST(ConfigTest, DateAndTimeTypes) {
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

TEST(ConfigTest, ArraysAndTables) {
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

TEST(ConfigTest, ArrayTableElementsReadableAsTables) {
    Config config;
    config.Load(kToml);

    // asArray + asTable 组合路径（asArrayTable 直读见 ArrayTable 用例，D8 已修）
    auto items = config.get("array_table_test.items")->asArray();
    ASSERT_TRUE(items.has_value());
    ASSERT_EQ(items->size(), 2U);
    EXPECT_EQ((*items)[0]->asTable()->at("id")->asInteger().value(), 1);
    EXPECT_EQ((*items)[1]->asTable()->at("name")->asString().value(), "item2");
}

TEST(ConfigTest, NestedPath) {
    Config config;
    config.Load(kToml);

    EXPECT_EQ(config.get("nested_test.server.host")->asString().value(), "localhost");
    EXPECT_EQ(config.get("nested_test.server.port")->asInteger().value(), 8080);
}

TEST(ConfigTest, MissingKeyReturnsNullValue) {
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

TEST(ConfigTest, TypeMismatchReturnsNullopt) {
    Config config;
    config.Load(kToml);

    EXPECT_FALSE(config.get("int_test.count")->asString().has_value());
    EXPECT_FALSE(config.get("string_test.name")->asInteger().has_value());
}

/// [D52] 未加载 get 不抛（用户裁决语义：未加载 ≡ 缺键，返回 NullValue）——
/// 原锁定用例 GetBeforeLoadThrows 随语义翻转
TEST(ConfigTest, GetBeforeLoadReturnsNullValue) {
    Config config;
    ASSERT_NO_FATAL_FAILURE([&] {
        auto value = config.get("any.key");
        ASSERT_NE(value, nullptr);
        EXPECT_FALSE(value->isString());
        EXPECT_FALSE(value->asString().has_value());
        EXPECT_FALSE(value->asInteger().has_value());
    }());
}

TEST(ConfigTest, InvalidTomlThrows) {
    Config config;
    EXPECT_THROW(config.Load("invalid = ["), Exception::ConfigError);
    EXPECT_FALSE(config.isLoaded());
}

TEST(ConfigTest, LoadFile) {
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

TEST(ConfigTest, ReloadReplacesValues) {
    Config config;
    config.Load("[app]\nname = \"first\"\n");
    EXPECT_EQ(config.get("app.name")->asString().value(), "first");

    config.Load("[app]\nname = \"second\"\n");
    EXPECT_EQ(config.get("app.name")->asString().value(), "second");
}

/// [正] Value 存活过 Config：派生值经 shared_ptr<Handle> 保持解析树存活，
///      标量与复合类型（asArray/asTable）均适用（Q29 闭环锁定——旧「move 浅拷贝
///      致悬垂」担忧属旧库形态，现所有权共享已根治）
TEST(ConfigTest, ValueOutlivesConfig) {
    Value::ptr value;
    Value::ptr arr;
    Value::ptr tab;
    {
        Config config;
        config.Load(kToml);
        value = config.get("int_test.count");
        arr = config.get("array_test.numbers");
        tab = config.get("string_test");
    }  // Config 已析构——派生值仍须可读
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->asInteger().value(), 42);
    auto nums = arr->asArray();
    ASSERT_TRUE(nums.has_value());
    EXPECT_FALSE(nums->empty());
    auto name = tab->asTable()->at("name")->asString();
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(name.value(), "hello world");
}

/// [正] asArrayTable 直读数组表（D8 修复）：元素为 table，内容可经 at 读取
TEST(ConfigTest, ArrayTable) {
    Config config;
    config.Load(kToml);

    auto items = config.get("array_table_test.items")->asArrayTable();
    ASSERT_TRUE(items.has_value());
    ASSERT_EQ(items->size(), 2U);
    EXPECT_EQ((*items)[0].at("id")->asInteger().value(), 1);
    EXPECT_EQ((*items)[1].at("name")->asString().value(), "item2");

    // 非数组键 → nullopt（类型不匹配语义与 asArray/asTable 一致）
    EXPECT_FALSE(config.get("string_test.name")->asArrayTable().has_value());
}

/// [界] 混入非 table 元素 → 整体 nullopt（D8 严格语义锁定：不静默丢弃）
TEST(ConfigTest, ArrayTableMixedElementsRejected) {
    Config config;
    config.Load("mixed = [1, {x = 2}]\n");

    EXPECT_FALSE(config.get("mixed")->asArrayTable().has_value());
}

// ---------------------------------------------------------------------------
// 新增场景（P10）
// ---------------------------------------------------------------------------

/// [并] 8 线程并发 get 同一与不同 key，取值一致（shared_mutex 读路径）
TEST(ConfigTest, ConcurrentGetIsConsistent) {
    Config config;
    config.Load(kToml);

    constexpr int kThreads = 8;
    constexpr int kReadsPerThread = 2000;
    std::atomic<int> mismatches{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t) {
        readers.emplace_back([&, t]() {
            for (int i = 0; i < kReadsPerThread; ++i) {
                // 同一 key
                auto name = config.get("string_test.name");
                if (!name->asString().has_value() || name->asString().value() != "hello world") {
                    mismatches.fetch_add(1);
                }
                // 不同 key（按线程错开）
                const std::string key = "array_test.numbers";
                if (t % 2 == 0) {
                    auto number = config.get(key);
                    if (!number->asArray().has_value()) {
                        mismatches.fetch_add(1);
                    }
                } else {
                    auto count = config.get("int_test.count");
                    if (!count->asInteger().has_value() || count->asInteger().value() != 42) {
                        mismatches.fetch_add(1);
                    }
                }
            }
        });
    }
    for (auto& thread : readers) {
        thread.join();
    }

    EXPECT_EQ(mismatches.load(), 0);
}

/// [错] LoadFile 不存在的文件抛 ConfigError
TEST(ConfigTest, LoadMissingFileThrows) {
    TempDir temp;
    Config config;
    EXPECT_THROW(config.LoadFile(temp.Path() / "no_such_file.toml"), Exception::ConfigError);
    EXPECT_FALSE(config.isLoaded());
}

/// [界] 空文档与仅注释文档：加载成功，任意 key 返回 NullValue
TEST(ConfigTest, EmptyDocumentLoads) {
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

/// [正] SYS_CONF 宏与 sys_config 全局对象冒烟（无 ./sys.toml 未加载时 ≡ 缺键，D52 语义）
TEST(ConfigTest, SysConfigMacroSmoke) {
    if (!sys_config.isLoaded()) {
        auto value = SYS_CONF("any.key");
        ASSERT_NE(value, nullptr);
        EXPECT_FALSE(value->asInteger().has_value());
        return;
    }
    // 已加载环境（例如其他用例 LoadFile 过）：宏等价于 sys_config.get
    auto value = SYS_CONF("string_test.name");
    EXPECT_EQ(value->asString().value_or("hello world"), "hello world");
}

}  // namespace test
}  // namespace bus
