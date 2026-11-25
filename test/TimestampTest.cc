//***********************************************
// name      : TimestampTest.cc
// brief     : 根级时间模块单元测试：被测 src/Timestamp.h/.cc（微秒时间戳）
// attention : Timestamp 默认构造不初始化内部值（Q17，源码 =default 无成员
//             初始化），默认构造对象的读取不做用例；缺 operator==/!=
//             （Q10）以编译期契约锁定
//***********************************************
#include <gtest/gtest.h>
#include "Exception.hpp"
#include "Timestamp.h"
#include "Util.h"

#include <concepts>
#include <sstream>
#include <string>

namespace bus {
namespace test {

namespace {

/// @brief 检测类型是否可用 operator== 比较
template <typename T>
concept HasEqual = requires(T a, T b) { a == b; };

}  // namespace

// Timestamp 提供完整比较语义：< 及其导出（>/≥/≤）+ ==/!=（Q10 已补齐）
static_assert(HasEqual<Timestamp>, "Timestamp 应提供 operator==（Q10 闭环锁定）");
static_assert(HasEqual<const Timestamp&>, "同上");

TEST(TimestampTest, NowAndUnits) {
    const Timestamp now = Timestamp::now();
    EXPECT_GT(now.getMicroSecond(), 0);
    EXPECT_EQ(now.getMilliSecond(), now.getMicroSecond() / 1000);
    EXPECT_EQ(now.getSecond(), now.getMicroSecond() / Timestamp::CONS_MicroSecondPerSecond);
}

TEST(TimestampTest, LiteralAndArithmetic) {
    Timestamp first = 1000_stamp;
    Timestamp second = 2000_stamp;

    EXPECT_EQ(first.getMicroSecond(), 1000);
    EXPECT_LT(first, second);
    EXPECT_TRUE(second > first);
    EXPECT_EQ((second - first).getMicroSecond(), 1000);
    EXPECT_EQ((second + first).getMicroSecond(), 3000);
    EXPECT_EQ((second / first).getMicroSecond(), 2);
    EXPECT_EQ((second % first).getMicroSecond(), 0);
}

TEST(TimestampTest, FormatUtc) {
    // 业务 epoch 1735660800000000us 对应 2025-01-01 00:00 UTC+8，
    // 即 UTC 时间 2024-12-31 16:00:00
    const Timestamp epoch = TimeUtil::getEpoch();
    EXPECT_EQ(epoch.toFormatString("%Y-%m-%d %H:%M:%S", false), "2024-12-31 16:00:00");
    EXPECT_EQ(epoch.toFormatString("%Y-%m-%d", false), "2024-12-31");
}

TEST(TimestampTest, DayBoundaries) {
    const Timestamp noon = TimeUtil::getEpoch() + Timestamp(12LL * 3600 * 1000000);
    const Timestamp day_start = noon.toDay();
    const Timestamp next_day = noon.nextDay();

    EXPECT_EQ(day_start.getMicroSecond() % Timestamp::CONS_MicroSecondPerDay, 0);
    EXPECT_EQ(next_day.getMicroSecond() - day_start.getMicroSecond(), Timestamp::CONS_MicroSecondPerDay);
    EXPECT_LE(day_start, noon);
    EXPECT_GT(next_day, noon);
}

TEST(TimestampTest, ToPosixTypes) {
    const Timestamp timestamp = 1735660800123456_stamp;
    EXPECT_EQ(timestamp.toTimespec().tv_sec, 1735660800LL);
    EXPECT_EQ(timestamp.toTimeval().tv_sec, 1735660800LL);
}

/// [正] toTimespec 的 tv_nsec 为秒内余数（μs % 1e6 × 1000）
TEST(TimestampTest, TimespecNanoseconds) {
    const Timestamp timestamp = 1735660800123456_stamp;
    EXPECT_EQ(timestamp.toTimespec().tv_nsec, 123456000L);
}

// ---------------------------------------------------------------------------
// 新增场景（P7）
// ---------------------------------------------------------------------------

/// [正] less_than_comparable 导出的全部序运算符组合
TEST(TimestampTest, DerivedOperatorCombinations) {
    const Timestamp low = 100_stamp;
    const Timestamp high = 200_stamp;
    const Timestamp same_low = 100_stamp;

    EXPECT_TRUE(low < high);
    EXPECT_TRUE(high > low);
    EXPECT_TRUE(low <= high);
    EXPECT_TRUE(high >= low);
    EXPECT_TRUE(low <= same_low);
    EXPECT_TRUE(low >= same_low);
    EXPECT_FALSE(high < low);
    EXPECT_FALSE(low > high);
    EXPECT_FALSE(low < same_low);
}

/// [正] ==/!= 补齐比较语义（Q10：less_than_comparable 仅导出 >/>=/<=）
TEST(TimestampTest, EqualityOperators) {
    const Timestamp a = 123456789_stamp;
    const Timestamp b = 123456789_stamp;
    const Timestamp c = 987654321_stamp;

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
    EXPECT_FALSE(a != b);
}

/// [界] 负数 epoch（1970 前）日界按 floor 除法归属（Q11 修复）：
/// -1μs 属 1969-12-31，toDay 归 -DAY、nextDay 归 0（1970-01-01 起点）
TEST(TimestampTest, NegativeEpochDayBoundaryFloors) {
    const Timestamp before_epoch(-1);

    EXPECT_EQ(before_epoch.toDay().getMicroSecond(), -86400000000LL);
    EXPECT_EQ(before_epoch.nextDay().getMicroSecond(), 0);

    // -DAY-5μs 属 1969-12-30：toDay 归 -2*DAY、nextDay 归 -DAY
    const Timestamp two_days_before(-86400000000LL - 5);
    EXPECT_EQ(two_days_before.toDay().getMicroSecond(), -172800000000LL);
    EXPECT_EQ(two_days_before.nextDay().getMicroSecond(), -86400000000LL);

    // 负侧恰好午夜整（-DAY 整数倍）不进位偏移
    const Timestamp neg_midnight(-86400000000LL);
    EXPECT_EQ(neg_midnight.toDay().getMicroSecond(), -86400000000LL);
    EXPECT_EQ(neg_midnight.nextDay().getMicroSecond(), 0);
}

/// [正] now() 单调不回退（连续两次采样，后者 >= 前者）
TEST(TimestampTest, NowIsMonotonic) {
    const Timestamp first = Timestamp::now();
    const Timestamp second = Timestamp::now();

    EXPECT_LE(first, second);
    EXPECT_GE(second, first);
}

/// [正] toTimeval 的 tv_usec 为秒内余数（μs % 1e6），恰整秒时为 0
TEST(TimestampTest, TimevalUsecTruncation) {
    const Timestamp timestamp = 1735660800123456_stamp;
    const timeval tv = timestamp.toTimeval();

    EXPECT_EQ(tv.tv_sec, 1735660800LL);
    EXPECT_EQ(tv.tv_usec, 123456);

    const Timestamp exact_second = 1735660800000000_stamp;
    EXPECT_EQ(exact_second.toTimeval().tv_usec, 0);
}

/// [界] 恰好午夜整（DAY 整数倍）的 toDay/nextDay 不进位偏移
TEST(TimestampTest, ExactMidnightDayBoundaries) {
    const int64_t day = Timestamp::CONS_MicroSecondPerDay;
    const Timestamp midnight = Timestamp(3 * day);  // 整三天

    EXPECT_EQ(midnight.toDay().getMicroSecond(), 3 * day);    // 自身即日界
    EXPECT_EQ(midnight.nextDay().getMicroSecond(), 4 * day);  // 次日日界
}

/// [正] 流输出微秒数值与 _stamp 字面量
TEST(TimestampTest, StreamOutputAndLiteral) {
    const Timestamp timestamp = 123456789_stamp;

    std::ostringstream os;
    os << timestamp;
    EXPECT_EQ(os.str(), "123456789");

    const auto literal = 987654321_stamp;
    EXPECT_EQ(literal.getMicroSecond(), 987654321);
}

}  // namespace test
}  // namespace bus
