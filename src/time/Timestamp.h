#pragma once
#include <base/copyable.hpp>
#include <base/less_than_comparable.hpp>
#include <base/Type.hpp>
#include <sys/types.h>
#include <string>
#include <sstream>

//***********************************************
// name      : Timestamp.h
// brief     : 时间戳[μs] -- UTC时间
// attention : None
// date      : 2023/10/11
//***********************************************
class Timestamp: copyable,
                 less_than_comparable<Timestamp>
{
public:
    /// @brief 每秒中的微秒单位
    static const int CONS_MicroSecondPerSecond=1000 * 1000;
    /// @brief 一天中的微秒单位
    static const int64_t CONS_MicroSecondPerDay=60ll*60ll*24ll*1000ll*1000ll;

    Timestamp()=default;
    constexpr explicit Timestamp(int64_t microSecondSinceEpoch): m_microSecondEpoch(microSecondSinceEpoch) {}

    std::string toFormatString(std::string&& fmt,bool isLocal=true) const
    {return toFormatString(fmt,isLocal);}
    std::string toFormatString(std::string& fmt,bool isLocal=true) const;
    std::string toFormatString() const
    {return toFormatString("");}

    /// @brief 获取当前时间[μs]
    /// @return
    static Timestamp now();
    /// @brief 下一日时间戳--UTC
    Timestamp nextDay() const
    {return static_cast<Timestamp>(m_microSecondEpoch/CONS_MicroSecondPerDay*CONS_MicroSecondPerDay+CONS_MicroSecondPerDay);}
    /// @brief 当日起始时间戳
    /// @return
    Timestamp toDay() const
    {return static_cast<Timestamp>(m_microSecondEpoch/CONS_MicroSecondPerDay*CONS_MicroSecondPerDay);}

    time_t getMicroSecond() const
    {return m_microSecondEpoch;}

    time_t getSecond() const
    {return m_microSecondEpoch/CONS_MicroSecondPerSecond;}

    /// @brief 时间类型转换
    timespec toTimespec() const;
    timeval toTimeval() const;


private:
    /// @brief 时间戳[μs]
    int64_t m_microSecondEpoch;
};

/// @brief 字面量时间戳
inline Timestamp operator""_stamp(unsigned long long int timestamp)
{
    return Timestamp(timestamp);
}

inline bool operator<(const Timestamp lhs , const Timestamp rhs)
{
    return lhs.getMicroSecond() < rhs.getMicroSecond();
}

inline Timestamp operator+(const Timestamp lhs , const Timestamp rhs)
{
    return Timestamp(lhs.getMicroSecond() + rhs.getMicroSecond());
}

inline Timestamp operator-(const Timestamp lhs , const Timestamp rhs)
{
    return Timestamp(lhs.getMicroSecond() - rhs.getMicroSecond());
}

inline Timestamp operator*(const Timestamp lhs , const Timestamp rhs)
{
    return Timestamp(lhs.getMicroSecond() * rhs.getMicroSecond());
}

inline Timestamp operator/(const Timestamp lhs , const Timestamp rhs)
{
    return Timestamp(lhs.getMicroSecond() / rhs.getMicroSecond());
}

inline Timestamp operator%(const Timestamp lhs , const Timestamp rhs)
{
    return Timestamp(lhs.getMicroSecond() % rhs.getMicroSecond());
}

inline std::ostream& operator<<(std::ostream& lhs ,const Timestamp rhs)
{
    return lhs<< rhs.getMicroSecond();
}