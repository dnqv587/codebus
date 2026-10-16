//***********************************************
// name      : ConfigTestNew.cc
// brief     : Config4Toml 模块 gtest 测试用例
// attention : 使用 gtest 框架
//***********************************************
#include <gtest/gtest.h>
#include "Config.h"
#include "exception/Exception.hpp"
#include <fstream>
#include <filesystem>

namespace bus {

// class ConfigTest : public ::testing::Test
// {
//  protected:
//     void SetUp() override;
//     void TearDown() override;

//     std::string m_testConfigPath;
// };

// void ConfigTest::SetUp()
// {
//     m_testConfigPath = "conf/test.toml";
// }

// void ConfigTest::TearDown()
// {
//     //std::filesystem::remove(m_testConfigPath);
// }

// // 测试配置文件内容
// static constexpr std::string_view g_testConfig = R"(
// [string_test]
// name = "hello world"
// message = 'escaped "string"'

// [int_test]
// count = 42
// negative = -100
// zero = 0

// [float_test]
// pi = 3.14159
// negative = -2.71828
// zero = 0.0

// [bool_test]
// enabled = true
// disabled = false

// [date_test]
// created = 2025-01-19

// [time_test]
// alarm = 14:30:00.123

// [datetime_test]
// timestamp = 2025-01-19T14:30:00

// [array_test]
// numbers = [1, 2, 3, 4, 5]
// names = ["alice", "bob", "charlie"]
// empty = []

// [table_test]
// key = "value"
// number = 123

// [array_table_test]
// items = [
//     { id = 1, name = "item1" },
//     { id = 2, name = "item2" },
//     { id = 3, name = "item3" }
// ]

// [nested_test]
// server = { host = "localhost", port = 8080 }
// )";

// // 测试：加载配置文件
// TEST_F(ConfigTest, LoadFile_Success)
// {
//     // 写入测试配置文件
//     std::ofstream configFile(m_testConfigPath);
//     configFile << g_testConfig;
//     configFile.close();

//     Config4Toml config("test");
//     EXPECT_NO_THROW(config.LoadFile(m_testConfigPath));
// }

// // 测试：加载配置字符串
// TEST_F(ConfigTest, LoadString_Success)
// {
//     Config4Toml config("test");
//     EXPECT_NO_THROW(config.Load(g_testConfig));
// }

// // 测试：获取字符串值
// TEST_F(ConfigTest, GetString_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto nameVal = config.get("string_test.name");
//     ASSERT_NE(nameVal, nullptr);
//     EXPECT_TRUE(nameVal->isString());
//     auto nameOpt = nameVal->asString();
//     ASSERT_TRUE(nameOpt.has_value());
//     EXPECT_EQ(nameOpt.value(), "hello world");
// }

// // 测试：获取整数值
// TEST_F(ConfigTest, GetInteger_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto countVal = config.get("int_test.count");
//     ASSERT_NE(countVal, nullptr);
//     EXPECT_TRUE(countVal->isInteger());
//     auto countOpt = countVal->asInteger();
//     ASSERT_TRUE(countOpt.has_value());
//     EXPECT_EQ(countOpt.value(), 42);

//     auto negativeVal = config.get("int_test.negative");
//     ASSERT_NE(negativeVal, nullptr);
//     EXPECT_TRUE(negativeVal->isInteger());
//     auto negativeOpt = negativeVal->asInteger();
//     ASSERT_TRUE(negativeOpt.has_value());
//     EXPECT_EQ(negativeOpt.value(), -100);
// }

// // 测试：获取浮点值
// TEST_F(ConfigTest, GetFloat_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto piVal = config.get("float_test.pi");
//     ASSERT_NE(piVal, nullptr);
//     EXPECT_TRUE(piVal->isFloat());
//     auto piOpt = piVal->asFloat();
//     ASSERT_TRUE(piOpt.has_value());
//     EXPECT_DOUBLE_EQ(piOpt.value(), 3.14159);
// }

// // 测试：获取布尔值
// TEST_F(ConfigTest, GetBoolean_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto enabledVal = config.get("bool_test.enabled");
//     ASSERT_NE(enabledVal, nullptr);
//     EXPECT_TRUE(enabledVal->isBoolean());
//     auto enabledOpt = enabledVal->asBoolean();
//     ASSERT_TRUE(enabledOpt.has_value());
//     EXPECT_TRUE(enabledOpt.value());

//     auto disabledVal = config.get("bool_test.disabled");
//     ASSERT_NE(disabledVal, nullptr);
//     EXPECT_TRUE(disabledVal->isBoolean());
//     auto disabledOpt = disabledVal->asBoolean();
//     ASSERT_TRUE(disabledOpt.has_value());
//     EXPECT_FALSE(disabledOpt.value());
// }

// // 测试：获取日期值
// TEST_F(ConfigTest, GetDate_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto dateVal = config.get("date_test.created");
//     ASSERT_NE(dateVal, nullptr);
//     EXPECT_TRUE(dateVal->isDate());
//     auto dateOpt = dateVal->asDate();
//     ASSERT_TRUE(dateOpt.has_value());
//     EXPECT_EQ(static_cast<int>(dateOpt.value().year()), 2025);
//     EXPECT_EQ(static_cast<unsigned>(dateOpt.value().month()), 1);
//     EXPECT_EQ(static_cast<unsigned>(dateOpt.value().day()), 19);
// }

// // 测试：获取时间值
// TEST_F(ConfigTest, GetTime_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto timeVal = config.get("time_test.alarm");
//     ASSERT_NE(timeVal, nullptr);
//     EXPECT_TRUE(timeVal->isTime());
//     auto timeOpt = timeVal->asTime();
//     ASSERT_TRUE(timeOpt.has_value());
//     EXPECT_EQ(timeOpt.value().hours().count(), 14);
//     EXPECT_EQ(timeOpt.value().minutes().count(), 30);
//     EXPECT_EQ(timeOpt.value().seconds().count(), 0);
// }

// // 测试：获取数组值
// TEST_F(ConfigTest, GetArray_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto numbersVal = config.get("array_test.numbers");
//     ASSERT_NE(numbersVal, nullptr);
//     EXPECT_TRUE(numbersVal->isArray());
//     auto numbersOpt = numbersVal->asArray();
//     ASSERT_TRUE(numbersOpt.has_value());
//     const auto& numbers = numbersOpt.value();
//     EXPECT_EQ(numbers.size(), 5);
//     EXPECT_EQ(numbers[0]->asInteger().value(), 1);
//     EXPECT_EQ(numbers[1]->asInteger().value(), 2);
//     EXPECT_EQ(numbers[2]->asInteger().value(), 3);
//     EXPECT_EQ(numbers[3]->asInteger().value(), 4);
//     EXPECT_EQ(numbers[4]->asInteger().value(), 5);
// }

// // 测试：获取空数组
// TEST_F(ConfigTest, GetEmptyArray_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto emptyVal = config.get("array_test.empty");
//     ASSERT_NE(emptyVal, nullptr);
//     EXPECT_TRUE(emptyVal->isArray());
//     auto emptyOpt = emptyVal->asArray();
//     ASSERT_TRUE(emptyOpt.has_value());
//     EXPECT_EQ(emptyOpt.value().size(), 0);
// }

// // 测试：获取表值
// TEST_F(ConfigTest, GetTable_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto tableVal = config.get("table_test");
//     ASSERT_NE(tableVal, nullptr);
//     EXPECT_TRUE(tableVal->isTable());
//     auto tableOpt = tableVal->asTable();
//     ASSERT_TRUE(tableOpt.has_value());
//     const auto& table = tableOpt.value();
//     EXPECT_EQ(table.size(), 2);

//     auto keyVal = table.find("key");
//     ASSERT_NE(keyVal, table.end());
//     EXPECT_TRUE(keyVal->second->isString());
//     EXPECT_EQ(keyVal->second->asString().value(), "value");

//     auto numberVal = table.find("number");
//     ASSERT_NE(numberVal, table.end());
//     EXPECT_TRUE(numberVal->second->isInteger());
//     EXPECT_EQ(numberVal->second->asInteger().value(), 123);
// }

// // 测试：获取数组表
// TEST_F(ConfigTest, GetArrayTable_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto itemsVal = config.get("array_table_test.items");
//     ASSERT_NE(itemsVal, nullptr);
//     EXPECT_TRUE(itemsVal->isArray());
//     auto arrayTableOpt = itemsVal->asArrayTable();
//     ASSERT_TRUE(arrayTableOpt.has_value());
//     const auto& arrayTable = arrayTableOpt.value();
//     EXPECT_EQ(arrayTable.size(), 3);

//     const auto& item1 = arrayTable[0];
//     EXPECT_EQ(item1.size(), 2);
//     auto id1 = item1.find("id");
//     ASSERT_NE(id1, item1.end());
//     EXPECT_EQ(id1->second->asInteger().value(), 1);
// }

// // 测试：获取嵌套配置
// TEST_F(ConfigTest, GetNestedConfig_Success)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto hostVal = config.get("nested_test.server.host");
//     ASSERT_NE(hostVal, nullptr);
//     EXPECT_TRUE(hostVal->isString());
//     EXPECT_EQ(hostVal->asString().value(), "localhost");

//     auto portVal = config.get("nested_test.server.port");
//     ASSERT_NE(portVal, nullptr);
//     EXPECT_TRUE(portVal->isInteger());
//     EXPECT_EQ(portVal->asInteger().value(), 8080);
// }

// // 测试：不存在的键返回 NullValue
// TEST_F(ConfigTest, GetNonExistentKey_ReturnsNullValue)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto val = config.get("nonexistent.key");
//     ASSERT_NE(val, nullptr);
//     EXPECT_FALSE(val->isString());
//     EXPECT_FALSE(val->isInteger());
//     EXPECT_FALSE(val->isFloat());
//     EXPECT_FALSE(val->isBoolean());
//     EXPECT_FALSE(val->isDate());
//     EXPECT_FALSE(val->isTime());
//     EXPECT_FALSE(val->isDateTime());
//     EXPECT_FALSE(val->isArray());
//     EXPECT_FALSE(val->isTable());

//     EXPECT_FALSE(val->asString().has_value());
//     EXPECT_FALSE(val->asInteger().has_value());
// }

// // 测试：类型不匹配返回 nullopt
// TEST_F(ConfigTest, TypeMismatch_ReturnsNullopt)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     auto stringVal = config.get("int_test.count");
//     ASSERT_NE(stringVal, nullptr);
//     // 尝试将整数值作为字符串获取
//     auto stringOpt = stringVal->asString();
//     EXPECT_FALSE(stringOpt.has_value());

//     // 尝试将字符串值作为整数获取
//     auto intVal = config.get("string_test.name");
//     ASSERT_NE(intVal, nullptr);
//     auto intOpt = intVal->asInteger();
//     EXPECT_FALSE(intOpt.has_value());
// }

// // 测试：未加载配置时抛出异常
// TEST_F(ConfigTest, GetBeforeLoad_ThrowsException)
// {
//     Config4Toml config("test");

//     EXPECT_THROW(
//         config.get("string_test.name"),
//         Exception::ConfigUnLoad
//     );
// }

// // 测试：缓存功能
// TEST_F(ConfigTest, Cache_CachesValues)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     // 第一次获取
//     auto val1 = config.get("string_test.name");
//     ASSERT_NE(val1, nullptr);

//     // 第二次获取（应该从缓存返回）
//     auto val2 = config.get("string_test.name");
//     ASSERT_NE(val2, nullptr);

//     // 验证两次获取的是同一个值对象（通过 shared_ptr）
//     EXPECT_EQ(val1, val2);
// }

// // 测试：重新加载清空缓存
// TEST_F(ConfigTest, Reload_ClearsCache)
// {
//     Config4Toml config("test");
//     config.Load(g_testConfig);

//     // 第一次加载后获取
//     auto val1 = config.get("string_test.name");
//     ASSERT_NE(val1, nullptr);

//     // 重新加载
//     std::ofstream configFile(m_testConfigPath);
//     configFile << R"([modified]
// key = "new value")";
//     configFile.close();
//     config.LoadFile(m_testConfigPath);

//     // 重新获取（应该创建新的值对象）
//     auto val2 = config.get("modified.key");
//     ASSERT_NE(val2, nullptr);
//     EXPECT_EQ(val2->asString().value(), "new value");
// }

} // namespace bus

// gtest 主函数
int main(int argc, char** argv)
{
    using namespace bus;
    // ::testing::InitGoogleTest(&argc, argv);
    // return RUN_ALL_TESTS();
    Value::ptr t;
    Value::ptr tt;
    {
        Config conf;
        conf.LoadFile("./conf/test.toml");
        std::cout<<conf.get("string_test.name")->asString().value()<<std::endl;
        std::cout<<conf.get("float_test.pi")->asFloat().value()<<std::endl;
        std::cout<<std::boolalpha<<conf.get("bool_test.enabled")->asBoolean().value()<<std::endl;
        std::cout<<conf.get("date_test.created")->asDate().value()<<std::endl;
        std::cout<<conf.get("time_test.alarm")->asTime().value()<<std::endl;
        std::cout<<conf.get("datetime_test.timestamp")->asDateTime().value()<<std::endl;

        auto ft = conf.get("nested_test.server.port");
        t.swap(ft);

        auto arr = conf.get("array_test.names")->asArray().value();
        for(auto c:arr){
            std::cout <<"arr:"<<c->asString().value()<<std::endl;
        }

        auto tab = conf.get("float_test")->asTable().value();
        for(auto c:tab){
            std::cout <<"table:"<<c.first<<":"<<c.second->asFloat().value()<<std::endl;
        }
        auto sss = conf.get("array_test.names");
        tt.swap(sss);
    }
    std::cout<<"test:"<<t->asInteger().value()<<std::endl;
    t.reset();
    auto sv = tt->asArray().value().at(0);
    std::cout<<"test2:"<<sv->asString().value()<<std::endl;
    tt.reset();
    std::cout<<"test3:"<<sv->asString().value()<<std::endl;
    sv.reset();
    //std::cout<<"test4:"<<sv->asString().value()<<std::endl;
    
}
