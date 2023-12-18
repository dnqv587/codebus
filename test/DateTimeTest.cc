#include <time/DateTime.h>
#include <iostream>

int main()
{
    Date d(2023,2,17);
    Time t(12,50,1,5050);
    TimeZoneOffset off(8);
    //std::cout<<d<<" "<<t<<off.toString()<<std::endl;
    DateTime::useSpaceSign();
    DateTime dt(d,t);
    std::cout<<dt<<std::endl;
    return 0;
}
