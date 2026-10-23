//***********************************************
// name      : AddressTest.cc
// brief     : net 模块单元测试：被测 src/net/Address.h/.cc（地址构造/转换/sockaddr_cast）
// attention : 端口断言要求 1..65534；不进行真实网络连接；死亡测试需 Debug
//             构建（assert 未被 NDEBUG 关闭）；本目标含 D17/D20/O3 禁用
//             用例与 Q7 现状锁定用例
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "net/Address.h"
#include "exception/Exception.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <csignal>
#include <string>

namespace bus
{
namespace test
{

TEST(AddressTest, IPv4Fields)
{
    Address address("127.0.0.1", 8899);

    EXPECT_EQ(address.port(), 8899);
    EXPECT_EQ(address.family(), AF_INET);
    EXPECT_FALSE(address.isIPV6());
    EXPECT_EQ(address.Length(), sizeof(sockaddr_in));
    EXPECT_EQ(address.getSockAddr()->sa_family, AF_INET);
    EXPECT_EQ(address.getSockAddrInet4()->sin_port, htons(8899));
    EXPECT_EQ(address.ipString(), "127.0.0.1");
}

TEST(AddressTest, IPv6Fields)
{
    Address address("::1", 8899, true);

    EXPECT_EQ(address.port(), 8899);
    EXPECT_EQ(address.family(), AF_INET6);
    EXPECT_TRUE(address.isIPV6());
    EXPECT_EQ(address.Length(), sizeof(sockaddr_in6));
    EXPECT_EQ(address.getSockAddr()->sa_family, AF_INET6);
    EXPECT_EQ(address.getSockAddrInet6()->sin6_port, htons(8899));
    EXPECT_EQ(address.ipString(), "::1");
}

TEST(AddressTest, WildcardAddress)
{
    Address address("*", 8080);
    EXPECT_EQ(address.ipString(), "0.0.0.0");
    EXPECT_EQ(address.getSockAddrInet4()->sin_addr.s_addr, INADDR_ANY);
}

/// [错] 非法 IP 字符串（v4/v6）构造抛 AddressError
TEST(AddressTest, InvalidIpThrows)
{
    EXPECT_THROW(Address("not.an.ip", 80), Exception::AddressError);
    EXPECT_THROW(Address("::gggg", 80, true), Exception::AddressError);
}

/// [正] ipString 返回无 NUL 填充的紧凑字符串（v4/v6）
TEST(AddressTest, IpStringHasNoPadding)
{
    Address v4("127.0.0.1", 8899);
    EXPECT_EQ(v4.ipString(), "127.0.0.1");

    Address v6("::1", 8899, true);
    EXPECT_EQ(v6.ipString(), "::1");
}

/// [正] toString 输出 ip:port 形式
TEST(AddressTest, ToStringHasColon)
{
    Address address("127.0.0.1", 8899);
    EXPECT_EQ(address.toString(), "127.0.0.1:8899");
}

TEST(AddressTest, SockaddrCast)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;

    sockaddr* generic = sockaddr_cast<sockaddr>(&address);
    ASSERT_NE(generic, nullptr);
    EXPECT_EQ(generic->sa_family, AF_INET);

    sockaddr_in* back = sockaddr_cast<sockaddr_in>(generic);
    EXPECT_EQ(back, &address);
}

// ---------------------------------------------------------------------------
// 新增场景（P2）
// ---------------------------------------------------------------------------

/// [界] 端口边界：1 与 65535（构造契约 assert(port > 0)；65535 为合法上限，
///      I2 契约修正——旧断言 port<65535 误排除 65535）的 port() 往返
TEST(AddressTest, PortBoundariesRoundTrip)
{
    Address lower("127.0.0.1", 1);
    EXPECT_EQ(lower.port(), 1U);

    Address upper("127.0.0.1", 65535);
    EXPECT_EQ(upper.port(), 65535U);
}

/// [界] 端口 0 触发构造 assert（abort）——死亡测试，锁定下界契约
///      （65535 已修正为合法上限，见 PortBoundariesRoundTrip；I2）
TEST(AddressTest, PortOutOfBoundsAborts)
{
    if (CODEBUS_TEST_UNDER_SANITIZER)
    {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            Address address("127.0.0.1", 0);
            (void)address;
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

// TODO(Q7): Address 默认构造后 variant 持零值 sockaddr_in（family()==0 即
// AF_UNSPEC），无 invalid 语义（src/net/Address.h:18）；本用例锁定当前行为，
// 若确认应增加显式 invalid 状态，修复后改为正向断言。
TEST(AddressTest, DefaultConstructedContract)
{
    Address address;
    EXPECT_EQ(address.family(), 0U); // AF_UNSPEC
    EXPECT_FALSE(address.isIPV6());
    EXPECT_EQ(address.Length(), sizeof(sockaddr_in));
    EXPECT_EQ(address.port(), 0U);
    EXPECT_EQ(address.getSockAddr()->sa_family, 0U);
}

} // namespace test
} // namespace bus
