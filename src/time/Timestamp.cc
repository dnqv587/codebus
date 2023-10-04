#include <time/Timestamp.h>
#include <sys/time.h>

Timestamp Timestamp::now()
{
    timeval tv;
    time_t time = 0;
    if(::gettimeofday(&tv,NULL)==0)
    {
        time=tv.tv_sec*CONS_MicroSecondPerSecond+tv.tv_usec;
    }
    return Timestamp(time);
}

std::string Timestamp::toFormatString(std::string &fmt, bool isLocal) const
{
    char buf[64]={0};
    time_t second=m_microEpoch/CONS_MicroSecondPerSecond;
    struct tm tp;
    if(isLocal)
    {
        ::localtime_r(&second,&tp);
    }
    else
    {
        ::gmtime_r(&second,&tp);
    }

    ::strftime(buf,64,fmt.c_str(),&tp);
    return buf;
}


