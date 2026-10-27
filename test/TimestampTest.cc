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

// Timestamp 仅有 operator< 及其导出（>/≥/≤），未提供 ==/!=（Q10）
static_assert(!HasEqual<Timestamp>, "Timestamp 不应提供 operator==（现状锁定，Q10）");
static_assert(!HasEqual<const Timestamp&>, "同上");

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

// TODO(Q11): toDay/nextDay 使用整除向零取整（src/Timestamp.h:52-63），
// 负数 epoch（1970 前）的日界计算错位：-1μs（属 1969-12-31）的 toDay()
// 归到 0（1970-01-01 起点）。本用例锁定当前行为；若确认应支持负时间戳，
// 修复（floor 除法）后改为正向断言。
TEST(TimestampTest, NegativeEpochDayBoundaryLocksCurrent) {
    const Timestamp before_epoch(-1);

    // 现状：-1μs 整除 DAY 得 0，toDay() 返回 1970-01-01 起点而非 1969-12-31
    EXPECT_EQ(before_epoch.toDay().getMicroSecond(), 0);
    EXPECT_EQ(before_epoch.nextDay().getMicroSecond(), Timestamp::CONS_MicroSecondPerDay);
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
