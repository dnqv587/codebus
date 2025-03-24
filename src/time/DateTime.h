#pragma once
#include <cstdint>
#include <exception/Exception.hpp>
#include <log/Logger.h>
#include <algorithm>

namespace bus
{
///RFC 3339
class Date : copyable
{
 public:
	uint16_t year;
	uint8_t month;
	uint8_t day;

	Date() noexcept = default;

	constexpr Date(uint16_t year, uint8_t month, uint8_t day)
		: year(year), month(month), day(day)
	{
		//ASSERT_THROW(year>0,Exception::DateTimeFormatError("in Date format,year must be greater than 0"));

		ASSERT_THROW(month > 0 && month <= 12,
			Exception::DateTimeFormatError("in Date format,month must be between 1 and 12"));

		ASSERT_THROW(day > 0 && day <= getCurrentMonthMaxDay(),
			Exception::DateTimeFormatError("in Date format,Number of days incorrect"));
	}

	~Date() = default;

	/// @brief 判断当前年份是否为闰年
	bool isLeapYear() const noexcept;

	friend std::ostream& operator<<(std::ostream& lhs, const Date& rhs)
	{
		rhs.PrintFormatTime(lhs);
		return lhs;
	}

 private:
	/// @brief 当前月份最大天数
	[[nodiscard]] int8_t getCurrentMonthMaxDay() const noexcept;

	/// @brief 格式化输出
	void PrintFormatTime(std::ostream& os) const noexcept;
};

class Time : copyable
{
 public:
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint32_t nanoSecond;

	/// @brief 每秒中最大纳秒
	static const uint32_t CONST_MAXNANOSECOND = 1000 * 1000 * 1000;

	Time() noexcept = default;

	constexpr Time(uint8_t hour, uint8_t minute, uint8_t second, uint32_t nanoSecond)
		: hour(hour),
		  minute(minute),
		  second(second),
		  nanoSecond(nanoSecond)
	{
		ASSERT_THROW(hour < 24, Exception::DateTimeFormatError("in Date format,hours must be less than 24"));
		ASSERT_THROW(minute < 60, Exception::DateTimeFormatError("in Date format,minute must be less than 60"));
		ASSERT_THROW(second < 60, Exception::DateTimeFormatError("in Date format,second must be less than 60"));
		ASSERT_THROW(nanoSecond < CONST_MAXNANOSECOND,
			Exception::DateTimeFormatError("in Date format,second must be less than 1,000,000,000"));
	}

	~Time() = default;

	friend std::ostream& operator<<(std::ostream& lhs, const Time& rhs)
	{
		rhs.PrintFormatTime(lhs);
		return lhs;
	}
 private:
	/// @brief 格式化输出
	void PrintFormatTime(std::ostream& os) const noexcept;

};

//***********************************************
// class     : TimeZoneOffset
// brief     : 时区偏移值-UTC
// attention : None
//***********************************************
class TimeZoneOffset
{
 public:

	TimeZoneOffset() = default;

	constexpr explicit TimeZoneOffset(int16_t hour, int16_t minute)
		: _minute(static_cast<int16_t>(hour * 60 + minute))
	{
		ASSERT_THROW(_minute >= -(12 * 60) && _minute <= (12 * 60),
			Exception::DateTimeFormatError("the timezone is not valid "));
	}

	constexpr explicit TimeZoneOffset(int16_t minute)
		: _minute(minute)
	{
		ASSERT_THROW(_minute >= -(12 * 60) && _minute <= (12 * 60),
			Exception::DateTimeFormatError("the timezone is not valid "));
	}

	~TimeZoneOffset() = default;

	[[nodiscard]] std::string toString() const noexcept;

	friend std::ostream& operator<<(std::ostream& lhs, const TimeZoneOffset& rhs)
	{
		rhs.PrintFormatTime(lhs);
		return lhs;
	}
 private:
	/// @brief 格式化输出
	void PrintFormatTime(std::ostream& os) const noexcept;

	int16_t _minute;
};

class DateTime : copyable
{
 public:
	Date date;
	Time time;
	TimeZoneOffset offset;

	constexpr DateTime(const Date& d, const Time& t, const TimeZoneOffset& off)
		: date(d),
		  time(t),
		  offset(off)
	{
	}

	constexpr DateTime(Date&& d, Time&& t, TimeZoneOffset&& off)
		: date(std::forward<Date>(d)),
		  time(std::forward<Time>(t)),
		  offset(std::forward<TimeZoneOffset>(off))
	{
	}

	constexpr DateTime(const Date& d, const Time& t)
		: date(d),
		  time(t),
		  offset()
	{
	}

	constexpr DateTime(Date&& d, Time&& t)
		: date(std::forward<Date>(d)),
		  time(std::forward<Time>(t)),
		  offset()
	{
	}

	static void useSpaceSign() noexcept
	{
		s_sign = ' ';
	}

	static void revertSign() noexcept
	{
		s_sign = 'T';
	}

	friend std::ostream& operator<<(std::ostream& lhs, const DateTime& rhs)
	{
		rhs.PrintFormatTime(lhs);
		return lhs;
	}

 private:
	/// @brief 格式化输出
	void PrintFormatTime(std::ostream& os) const noexcept;

	static char s_sign;
};

}