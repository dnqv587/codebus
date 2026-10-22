//***********************************************
// name      : ServerStatusTest.cc
// brief     : net 模块单元测试：被测 src/net/ServerStatus.h（服务器状态机模板）
// attention : ServerStatus 不在 namespace bus（全局模板），引用需 :: 前缀；
//             isAtomic 模板参数当前无效（Q5，条件类型定义被注释）；
//             None（D5 已闭环）
//***********************************************
#include <gtest/gtest.h>
#include "net/ServerStatus.h"

#include <string_view>

namespace bus
{
namespace test
{

TEST(ServerStatusTest, DefaultStatus)
{
    ::ServerStatus<false> status;
    EXPECT_EQ(status.getStatus(), ::ServerStatus<false>::Status::Null);
    EXPECT_EQ(status.StatusToString(), "Null");
}

/// [正] setStatus 返回迁移前旧状态，新状态立即生效
TEST(ServerStatusTest, SetStatusReturnsStatus)
{
    ::ServerStatus<false> status;
    const auto previous =
        status.setStatus(::ServerStatus<false>::Status::Running); // 当前无 return，取值为 UB
    EXPECT_EQ(previous, ::ServerStatus<false>::Status::Null);
    EXPECT_EQ(status.getStatus(), ::ServerStatus<false>::Status::Running);
    EXPECT_EQ(status.StatusToString(), "Running");
}

// ---------------------------------------------------------------------------
// 新增场景（P2）
// ---------------------------------------------------------------------------

/// [正] 全部七态的 setStatus/getStatus/StatusToString 往返一致
TEST(ServerStatusTest, FullStateTransitionAndStrings)
{
    using S = ::ServerStatus<false>;
    S status;

    struct Row
    {
        S::Status value;
        std::string_view name;
    };
    const Row rows[] = {
        {S::Status::Null, "Null"},
        {S::Status::Initializing, "Initializing"},
        {S::Status::Initialized, "Initialized"},
        {S::Status::Running, "Running"},
        {S::Status::Stopping, "Stopping"},
        {S::Status::Stopped, "Stopped"},
        {S::Status::Destroy, "Destroy"},
    };

    for (const auto& row : rows)
    {
        status.setStatus(row.value);
        EXPECT_EQ(status.getStatus(), row.value);
        EXPECT_EQ(status.StatusToString(), row.name);
    }
}

/// [界] 重复设置同一状态与回退设置（模板未约束流转合法性，锁定自由语义）
TEST(ServerStatusTest, RepeatedAndBackwardTransitionAllowed)
{
    using S = ::ServerStatus<false>;
    S status;
    status.setStatus(S::Status::Running);
    status.setStatus(S::Status::Running); // 重复
    EXPECT_EQ(status.getStatus(), S::Status::Running);

    status.setStatus(S::Status::Initializing); // 回退（当前无合法性约束）
    EXPECT_EQ(status.getStatus(), S::Status::Initializing);
}

} // namespace test
} // namespace bus
