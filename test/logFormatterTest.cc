#include <iostream>
#include <log/Logger.h>

int main()
{
    Logger log("root");
    log.setLevel(LogLevel::WARN);
    Loggin(log,__FILE__,__LINE__,__FUNCTION__,LogLevel::WARN);
    return 0;
}
