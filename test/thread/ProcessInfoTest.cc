//***********************************************
// name      : ProcessInfoTest.cc
// brief     : thread 模块单元测试：被测 src/thread/ProcessInfo.h/.cc（进程标识）
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "thread/ProcessInfo.h"

#include <unistd.h>

#include <string>

namespace bus
{
namespace test
{

// ---------------------------------------------------------------------------
// 迁移用例（自旧 UtilTest）
// ---------------------------------------------------------------------------

TEST(ProcessInfoTest, PidAndHostname)
{
    EXPECT_EQ(ProcessInfo::Pid(), ::getpid());
    EXPECT_FALSE(ProcessInfo::hostname().empty());
}

// ---------------------------------------------------------------------------
// 新增场景（P8）
// ---------------------------------------------------------------------------

/// [正] Pid 同进程内稳定；hostname 非空且不含路径分隔符
TEST(ProcessInfoTest, PidStableHostnameSane)
{
    const pid_t first = ProcessInfo::Pid();
    EXPECT_EQ(ProcessInfo::Pid(), first);

    const std::string host = ProcessInfo::hostname();
    EXPECT_FALSE(host.empty());
    EXPECT_EQ(host.find('/'), std::string::npos);
    EXPECT_EQ(host, ProcessInfo::hostname()); // 稳定
}

} // namespace test
} // namespace bus
