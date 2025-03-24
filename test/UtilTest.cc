#include <bitset>
#include <iostream>

#include "util/Util.h"

using namespace bus;

int main() {
  // auto ret=SystemUtil::Backtrace(12,0);
  //std::cout << SystemUtil::BacktraceToString(100, 1, "backtrace:");
  std::cout<<std::bitset<64>(SystemUtil::generateSnowflakeKey())<<std::endl;
  static_assert(TimeUtil::getEpoch().getMilliSecond() == 1735660800000l);
  return 0;
}
