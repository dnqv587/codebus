#include <iostream>
#include <log/Logger.h>

int main()
{
    Logger log("root");
    log.setLevel(LogLevel::WARN);
    log.setFormatter("%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n %M%%--%h-%P");
    Logging(log, __FILE__, __LINE__, __FUNCTION__, LogLevel::WARN).getStream() << "Hello World!" << 123 << 0.1 << '[' << Timestamp::now().nextDay() << ']' << ::time(NULL);
    return 0;
}
