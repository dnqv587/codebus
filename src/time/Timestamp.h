#pragma once
#include <sys/types.h>

#include <string>
#include <sstream>

#include "base/copyable.hpp"
#include "base/less_than_comparable.hpp"
#include "base/type.h"
#include "base/Macro.h"

namespace bus
{
//***********************************************
// name      : Timestamp.h
// brief     : 时间戳[μs] -- UTC时间
// attention : None
// date      : 2023/10/11
//***********************************************
class Timestamp : copyable,
				  less_than_comparable<Timestamp>
{
 public:
	/// @brief 每秒中的微秒单位
	static const int CONS_MicroSecondPerSecond = 1000 * 1000;
	/// @brief 一天中的微秒单位
	static const int64_t CONS_MicroSecondPerDay = 60ll * 60ll * 24ll * 1000ll * 1000ll;

	Timestamp() = default;
	constexpr explicit Timestamp(int64_t microSecondSinceEpoch) : m_microSecondEpoch(microSecondSinceEpoch)
	{
	}
	ATTR_PURE_INLINE
	std::string toFormatString(std::string&& fmt, bool isLocal = true) const
	{
		return toFormatString(fmt, isLocal);
	}
	ATTR_NODISCARD
	std::string toFormatString(const std::string& fmt, bool isLocal = true) const;
	ATTR_NODISCARD
	std::string toFormatString() const
	{
		return toFormatString("");
	}

	/// @brief 获取当前时间[μs]
	/// @return
	ATTR_NODISCARD
	static Timestamp now();
	/// @brief 下一日时间戳--UTC
	ATTR_PURE_INLINE
	Timestamp nextDay() const
	{
		return static_cast<Timestamp>(m_microSecondEpoch / CONS_MicroSecondPerDay * CONS_MicroSecondPerDay
			+ CONS_MicroSecondPerDay);
	}
	/// @brief 当日起始时间戳
	/// @return
	ATTR_PURE_INLINE
	Timestamp toDay() const
	{
		return static_cast<Timestamp>(m_microSecondEpoch / CONS_MicroSecondPerDay * CONS_MicroSecondPerDay);
	}
	ATTR_PURE_INLINE
	time_t getMicroSecond() const
	{
		return m_microSecondEpoch;
	}
	ATTR_PURE_INLINE
	time_t getMilliSecond() const
	{
		return m_microSecondEpoch / 1000;
	}
	ATTR_PURE_INLINE
	time_t getSecond() const
	{
		return m_microSecondEpoch / CONS_MicroSecondPerSecond;
	}

	/// @brief 时间类型转换
	ATTR_NODISCARD
	timespec toTimespec() const;
	ATTR_NODISCARD
	timeval toTimeval() const;

 private:
	/// @brief 时间戳[μs]
	int64_t m_microSecondEpoch;
};

/// @brief 字面量时间戳
ATTR_INLINE
Timestamp operator ""_stamp(unsigned long long int timestamp)
{
	return Timestamp(timestamp);
}
ATTR_INLINE
bool operator<(const Timestamp lhs, const Timestamp rhs)
{
	return lhs.getMicroSecond() < rhs.getMicroSecond();
}
ATTR_INLINE
Timestamp operator+(const Timestamp lhs, const Timestamp rhs)
{
	return Timestamp(lhs.getMicroSecond() + rhs.getMicroSecond());
}
ATTR_INLINE
Timestamp operator-(const Timestamp lhs, const Timestamp rhs)
{
	return Timestamp(lhs.getMicroSecond() - rhs.getMicroSecond());
}
ATTR_INLINE
Timestamp operator*(const Timestamp lhs, const Timestamp rhs)
{
	return Timestamp(lhs.getMicroSecond() * rhs.getMicroSecond());
}
ATTR_INLINE
Timestamp operator/(const Timestamp lhs, const Timestamp rhs)
{
	return Timestamp(lhs.getMicroSecond() / rhs.getMicroSecond());
}
ATTR_INLINE
Timestamp operator%(const Timestamp lhs, const Timestamp rhs)
{
	return Timestamp(lhs.getMicroSecond() % rhs.getMicroSecond());
}
ATTR_INLINE
std::ostream& operator<<(std::ostream& lhs, const Timestamp rhs)
{
	return lhs << rhs.getMicroSecond();
}
}