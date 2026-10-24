//***********************************************
// name      : DateTimeTest.cc
// brief     : 根级时间模块单元测试：被测 src/DateTime.h/.cc（Date/Time/TimeZoneOffset/DateTime）
// attention : DateTime 的输出分隔符为全局静态状态（s_sign），fixture 负责
//             revertSign 恢复；TimeZoneOffset/Date/Time 默认构造不初始化
//             成员（Q18），默认构造对象的读取不做用例
//***********************************************
#include <gtest/gtest.h>
#include "Exception.hpp"
#include "DateTime.h"

#include <sstream>
#include <string>

namespace bus
{
namespace test
{

class DateTimeTest : public ::testing::Test
{
 protected:
    void TearDown() override
    {
        DateTime::revertSign();
    }
};

// ---------------------------------------------------------------------------
// Date
// ---------------------------------------------------------------------------

TEST(DateTest, ValidDateAndLeapYear)
{
    Date leap(2024, 2, 29);
    EXPECT_TRUE(leap.isLeapYear());
    EXPECT_FALSE(Date(2023, 2, 28).isLeapYear());

    std::ostringstream os;
    os << leap;
    EXPECT_EQ(os.str(), "2024-02-29");
}

TEST(DateTest, InvalidDateThrows)
{
    EXPECT_THROW(Date(2023, 2, 29), Exception::DateTimeFormatError);
    EXPECT_THROW(Date(2024, 0, 1), Exception::DateTimeFormatError);
    EXPECT_THROW(Date(2024, 13, 1), Exception::DateTimeFormatError);
    EXPECT_THROW(Date(2024, 4, 31), Exception::DateTimeFormatError);
}

// ---------------------------------------------------------------------------
// Time / TimeZoneOffset / DateTime（新增场景见文末）
// ---------------------------------------------------------------------------

TEST(TimeTest, ValidAndInvalidTime)
{
    std::ostringstream os;
    os << Time(14, 30, 0, 123000000);
    EXPECT_EQ(os.str(), "14:30:00.123");

    EXPECT_THROW(Time(24, 0, 0, 0), Exception::DateTimeFormatError);
    EXPECT_THROW(Time(0, 60, 0, 0), Exception::DateTimeFormatError);
    EXPECT_THROW(Time(0, 0, 60, 0), Exception::DateTimeFormatError);
    EXPECT_THROW(Time(0, 0, 0, 1000000000), Exception::DateTimeFormatError);
}

TEST(TimeZoneOffsetTest, FormatAndValidation)
{
    EXPECT_EQ(TimeZoneOffset(8, 0).toString(), "+08:00");
    EXPECT_EQ(TimeZoneOffset(0, 0).toString(), "Z");
    EXPECT_EQ(TimeZoneOffset(-5, -30).toString(), "-05:30");
    EXPECT_THROW(TimeZoneOffset(13, 0), Exception::DateTimeFormatError);
}

TEST_F(DateTimeTest, OutputContainsComponents)
{
    DateTime datetime(Date(2025, 1, 19), Time(14, 30, 0, 123000000), TimeZoneOffset(8, 0));

    std::ostringstream os;
    os << datetime;
    const std::string text = os.str();

    EXPECT_NE(text.find("2025-01-19"), std::string::npos);
    EXPECT_NE(text.find("14:30:00.123"), std::string::npos);
    EXPECT_NE(text.find("+08:00"), std::string::npos);
}

TEST_F(DateTimeTest, SpaceSignCanBeSwitched)
{
    DateTime datetime(Date(2025, 1, 19), Time(14, 30, 0, 0));
    DateTime::useSpaceSign();

    std::ostringstream os;
    os << datetime;
    EXPECT_NE(os.str().find("2025-01-19 14:30:00"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 新增场景（P7）
// ---------------------------------------------------------------------------

/// [界] 闰年三档边界：2000（世纪闰）、1900（非闰）、2024（普通闰）
TEST(DateTest, LeapYearBoundaries)
{
    EXPECT_TRUE(Date(2000, 2, 29).isLeapYear());  // 世纪闰（400 整除）
    EXPECT_THROW(Date(1900, 2, 29), Exception::DateTimeFormatError); // 世纪非闰
    EXPECT_TRUE(Date(2024, 2, 29).isLeapYear());  // 普通闰（4 整除）

    EXPECT_TRUE(Date(2000, 1, 1).isLeapYear());
    EXPECT_FALSE(Date(1900, 1, 1).isLeapYear());
    EXPECT_FALSE(Date(2023, 1, 1).isLeapYear());

    // TODO: year>0 的校验在源码中被注释（src/DateTime.h:22），year=0 可构造；
    // 若确认应校验，恢复断言后本行改为 EXPECT_THROW
    EXPECT_NO_THROW(Date(0, 1, 1));
}

/// [界] 各月天数边界：大月 31、小月 30、12/31 与次年衔接
TEST(DateTest, MonthDayBoundaries)
{
    EXPECT_NO_THROW(Date(2024, 1, 31));
    EXPECT_NO_THROW(Date(2024, 12, 31));
    EXPECT_NO_THROW(Date(2024, 4, 30));
    EXPECT_THROW(Date(2024, 4, 31), Exception::DateTimeFormatError); // 小月 31 非法
    EXPECT_THROW(Date(2024, 6, 31), Exception::DateTimeFormatError);
    EXPECT_THROW(Date(2024, 1, 32), Exception::DateTimeFormatError);
    EXPECT_THROW(Date(2024, 1, 0), Exception::DateTimeFormatError);

    std::ostringstream os;
    os << Date(2024, 12, 31);
    EXPECT_EQ(os.str(), "2024-12-31");
}

/// [界] Time 上边界 23:59:59.999999999 合法
TEST(TimeTest, UpperBoundary)
{
    std::ostringstream os;
    os << Time(23, 59, 59, 999999999);
    EXPECT_EQ(os.str(), "23:59:59.999999999");

    std::ostringstream midnight;
    midnight << Time(0, 0, 0, 0);
    EXPECT_EQ(midnight.str(), "00:00:00");
}

/// [界] 时区边界：±12:00 合法、超界抛出
TEST(TimeZoneOffsetTest, BoundaryValues)
{
    EXPECT_EQ(TimeZoneOffset(12, 0).toString(), "+12:00");
    EXPECT_EQ(TimeZoneOffset(-12, 0).toString(), "-12:00");
    EXPECT_THROW(TimeZoneOffset(12, 1), Exception::DateTimeFormatError);
    EXPECT_THROW(TimeZoneOffset(-12, -1), Exception::DateTimeFormatError);
    EXPECT_THROW(TimeZoneOffset(13, 0), Exception::DateTimeFormatError);
    EXPECT_THROW(TimeZoneOffset(-13, 0), Exception::DateTimeFormatError);
}

/// [正] DateTime 公开成员直访与组件一致性
TEST_F(DateTimeTest, ComponentAccess)
{
    DateTime datetime(Date(2025, 6, 15), Time(9, 45, 30, 500000000), TimeZoneOffset(-7, -30));

    EXPECT_EQ(datetime.date.year, 2025);
    EXPECT_EQ(datetime.date.month, 6);
    EXPECT_EQ(datetime.date.day, 15);
    EXPECT_EQ(datetime.time.hour, 9);
    EXPECT_EQ(datetime.time.minute, 45);
    EXPECT_EQ(datetime.time.second, 30);
    EXPECT_EQ(datetime.time.nanoSecond, 500000000U);
    EXPECT_EQ(datetime.offset.toString(), "-07:30");
}

} // namespace test
} // namespace bus
