#include <time/DateTime.h>
#include <iostream>

namespace uf
{
char DateTime::s_sign = 'T';

namespace detail
{
/// @brief 约束长度输出整形，不够补0
/// @param os 流
/// @param val 值
/// @param restrict 约束长度
template<typename T, typename=typename std::enable_if_t<std::is_integral_v<T>>>
void print_restrict_int(std::ostream& os, T val, int restrict)
{
	char str[restrict];
	for (int i = restrict - 1; i >= 0; --i)
	{
		uint8_t digit = val % 10u;
		val /= 10u;
		str[i] = static_cast<char>('0' + digit);
	}
	os.write(str, restrict);
}

/// @brief 自适应输出整形
template<typename T, typename=typename std::enable_if_t<std::is_integral_v<T>>>
void print_int(std::ostream& os, T val)
{
	int count = 0;
	T cpyVal = val;
	while (cpyVal)
	{
		cpyVal /= 10u;
		++count;
	}
	print_restrict_int(os, val, count);
}

}

bool Date::isLeapYear() const noexcept
{
	//闰年：1、能被4整除且不能被100整除
	//     2、能被400整除
	if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
	{
		return true;
	}
	return false;
}

int8_t Date::getCurrentMonthMaxDay() const noexcept
{
	const static auto BigMon = { 1, 3, 5, 7, 8, 10, 12 };
	if (2 == month)
	{
		return isLeapYear() ? 29 : 28;
	}
	else
	{
		if (std::find(BigMon.begin(), BigMon.end(), month) != BigMon.end())
		{
			return 31;
		}
		else
		{
			return 30;
		}
	}
}

void Time::PrintFormatTime(std::ostream& os) const noexcept
{
	//输入格式，例：23:01:59.1234
	detail::print_restrict_int(os, hour, 2);
	os.put(':');
	detail::print_restrict_int(os, minute, 2);
	os.put(':');
	detail::print_restrict_int(os, second, 2);
	if (nanoSecond && nanoSecond <= 999999999u)
	{
		os.put('.');
		auto ns = nanoSecond;
		int digits = 9u;
		while (ns % 10u == 0u)
		{
			ns /= 10u;
			digits--;
		}
		detail::print_restrict_int(os, ns, digits);
	}
}

void Date::PrintFormatTime(std::ostream& os) const noexcept
{
	//输入格式，例：2023-12-02
	detail::print_restrict_int(os, year, 4);
	os.put('-');
	detail::print_restrict_int(os, month, 2);
	os.put('-');
	detail::print_restrict_int(os, day, 2);
}

void TimeZoneOffset::PrintFormatTime(std::ostream& os) const noexcept
{
	if (_minute == 0)
	{
		os.put('Z');
	}
	else if (_minute > 0)
	{
		os.put('+');
		detail::print_restrict_int(os, _minute / 60, 2);
		os.put(':');
		detail::print_restrict_int(os, _minute % 60, 2);
	}
	else if (_minute < 0)
	{
		os.put('-');
		detail::print_restrict_int(os, std::abs(_minute) / 60, 2);
		os.put(':');
		detail::print_restrict_int(os, std::abs(_minute) % 60, 2);
	}

}
std::string TimeZoneOffset::toString() const noexcept
{
	std::stringstream time;
	PrintFormatTime(time);
	return time.str();
}

void DateTime::PrintFormatTime(std::ostream& os) const noexcept
{
	os << date << s_sign << time << offset;
}
}