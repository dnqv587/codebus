#include "time/Timestamp.h"

#include <sys/time.h>

namespace uf
{
Timestamp Timestamp::now()
{
	timeval tv{};
	time_t time = 0;
	if (::gettimeofday(&tv, nullptr) == 0)
	{
		time = tv.tv_sec * CONS_MicroSecondPerSecond + tv.tv_usec;
	}
	return Timestamp(time);
}

std::string Timestamp::toFormatString(const std::string& fmt, bool isLocal) const
{
	char buf[64] = { 0 };
	time_t second = m_microSecondEpoch / CONS_MicroSecondPerSecond;
	struct tm tp{};
	if (isLocal)
	{
		::localtime_r(&second, &tp);
	}
	else
	{
		::gmtime_r(&second, &tp);
	}

	::strftime(buf, 64, fmt.c_str(), &tp);
	return buf;
}

timespec Timestamp::toTimespec() const
{
	timespec t{};
	t.tv_sec = getSecond();
	t.tv_nsec = (m_microSecondEpoch - (m_microSecondEpoch % CONS_MicroSecondPerSecond)) * 1000;
	return t;
}

timeval Timestamp::toTimeval() const
{
	timeval t{};
	t.tv_sec = getSecond();
	t.tv_usec = m_microSecondEpoch - (m_microSecondEpoch % CONS_MicroSecondPerSecond);
	return t;
}
}

