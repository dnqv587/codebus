#pragma once
#include <base/copyable.hpp>
#include <base/less_than_comparable.hpp>
#include <sys/types.h>
#include <string>

class Timestamp:copyable,
                less_than_comparable<Timestamp>
{
public:
    /// @brief 每秒中的毫秒单位
    static const int CONS_MicroSecondPerSecond=1000 * 1000;

    Timestamp()=default;
    Timestamp(time_t microSecond):m_microEpoch(microSecond) {}

    std::string toFormatString(std::string&& fmt,bool isLocal=true) const
    {return toFormatString(fmt,isLocal);}
    std::string toFormatString(std::string& fmt,bool isLocal=true) const;
    std::string toFormatString() const
    {return toFormatString("");}

    /// @brief 获取当前时间[ms]
    /// @return
    static Timestamp now();

    time_t getMicroSecond() const
    {return m_microEpoch;}

    time_t getSecond() const
    {return m_microEpoch/CONS_MicroSecondPerSecond;}


private:
    /// @brief 时间戳[ms]
    time_t m_microEpoch;
};

inline bool operator<(const Timestamp lhs ,const Timestamp rhs)
{
    return lhs.getMicroSecond()<rhs.getMicroSecond();
}