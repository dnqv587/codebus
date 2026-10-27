//***********************************************
// name      : TimeUtilTest.cc
// brief     : 根级工具模块单元测试：被测 src/Util.h（TimeUtil 命名空间）
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "Timestamp.h"
#include "Util.h"

namespace bus {
namespace test {

TEST(TimeUtilTest, CustomEpoch) {
    static_assert(TimeUtil::getEpoch().getMicroSecond() == 1735660800000000LL, "custom epoch must be 2025-01-01");
    const Timestamp epoch = TimeUtil::getEpoch();
    EXPECT_EQ(epoch.getSecond(), 1735660800LL);
    EXPECT_EQ(epoch.getMilliSecond(), 1735660800000LL);
}

TEST(TimeUtilTest, GetElapse) {
    const Timestamp elapse = TimeUtil::getElapse();
    EXPECT_GE(elapse.getMicroSecond(), 0);
}

/// [正] getElapse 随运行时间不回退
TEST(TimeUtilTest, GetElapseMonotonic) {
    const Timestamp first = TimeUtil::getElapse();
    const Timestamp second = TimeUtil::getElapse();
    EXPECT_LE(first, second);
}

}  // namespace test
}  // namespace bus
