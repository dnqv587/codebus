//***********************************************
// name      : MacroTest.cc
// brief     : base 模块契约测试：被测 src/base/Macro.h（LIKELY/CONSTEVAL/CASE_STRING/ASSERT）
// attention : ASSERT(false) 经 assert_fail 调 abort——死亡测试需 Debug 构建，
//             sanitizer 构建下由守卫宏跳过；ATTR_* 纯编译期宏以编译通过覆盖
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "base/Macro.h"

#include <csignal>
#include <string_view>

namespace bus
{
namespace test
{

namespace
{

// CASE_STRING 宏展开使用裸枚举名（case kRed），仅适用于非 scoped enum
enum Color
{
    kRed,
    kGreen,
};

constexpr std::string_view ColorToString(Color color)
{
    CASE_STRING_BEGIN(color)
    CASE_STRING(kRed)
    CASE_STRING(kGreen)
    CASE_STRING_END()
}

} // namespace

/// [正] LIKELY/UNLIKELY 传递条件真值（!! 会将非零值归一为 1）
TEST(MacroTest, LikelyUnlikelyPassThrough)
{
    EXPECT_TRUE(LIKELY(1 + 1 == 2));
    EXPECT_FALSE(LIKELY(1 + 1 == 3));
    EXPECT_TRUE(UNLIKELY(1 + 1 == 2));
    EXPECT_FALSE(UNLIKELY(1 + 1 == 3));

    // 宏体为 __builtin_expect(!!(x), 1)：非零值经 !! 布尔化归一为 1
    EXPECT_EQ(LIKELY(42), 1);
    EXPECT_EQ(UNLIKELY(42), 1);
    EXPECT_EQ(LIKELY(0), 0);
}

/// [正] CONSTEVAL/GET_CONSTEVAL 求值
TEST(MacroTest, ConstevalEvaluates)
{
    EXPECT_EQ(GET_CONSTEVAL(CONSTEVAL(6 * 7)), 42);

    const std::string text = GET_CONSTEVAL(CONSTEVAL(std::string("ok")));
    EXPECT_EQ(text, "ok");
}

/// [正] CASE_STRING 映射枚举为字符串
/// @attention 未匹配值走 default 返回 "Unkown"（源码既有拼写，历史保留）
TEST(MacroTest, CaseStringMapsEnum)
{
    EXPECT_EQ(ColorToString(Color::kRed), "kRed");
    EXPECT_EQ(ColorToString(Color::kGreen), "kGreen");
    EXPECT_EQ(ColorToString(static_cast<Color>(99)), "Unkown"); // 现状：default 拼写
}

/// [正] ASSERT 条件成立时无副作用
TEST(MacroTest, AssertTrueSurvives)
{
    ASSERT(true, "不应触发");
    int executed = 0;
    ASSERT(++executed == 1, "不应触发");
    EXPECT_EQ(executed, 1);
}

/// [错] ASSERT 条件为假时打印并 abort——死亡测试
TEST(MacroTest, AssertFalseAborts)
{
    if (CODEBUS_TEST_UNDER_SANITIZER)
    {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            ASSERT(1 + 1 == 3, "custom message");
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

} // namespace test
} // namespace bus
