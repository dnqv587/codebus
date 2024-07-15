#include <iostream>
#include "util/Util.h"


int main()
{
    //auto ret=SystemUtil::Backtrace(12,0);
    std::cout<<SystemUtil::BacktraceToString(100,1,"backtrace:");
    return 0;
}
