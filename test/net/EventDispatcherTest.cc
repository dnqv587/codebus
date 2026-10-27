//***********************************************
// name      : EventDispatcherTest.cc
// brief     : net 模块单元测试：被测 src/net/EventDispatcher.h/.cc（epoll 封装：注册/修改/移除/分发）
// attention : EventLoop 的 m_dispatcher 为私有成员且无访问接口，本目标在创建
//             EventLoop 的同一线程内自建 standalone EpollEventDispatcher，
//             直接调用其公有 UpdateChannel()/Dispatcher() 验证 epoll 协议
//             （Channel 的 Enable/Disable 会同时注册进 loop 自身的 dispatcher，
//             两套 epoll 互不干扰；经 EventLoop::Loop 间接驱动的路径由
//             EventLoopTest 覆盖）；本目标含 D14 禁用用例
//***********************************************
#include <gtest/gtest.h>
#include "TestHelper.h"
#include "net/Channel.h"
#include "net/EventDispatcher.h"
#include "net/EventLoop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <vector>

namespace bus {
namespace test {

class EventDispatcherTest : public ::testing::Test {
 protected:
    void SetUp() override {
        // EventLoop 仅构造不 Loop：UpdateChannel 的线程断言要求在创建线程执行
        m_loop = std::make_unique<EventLoop>();
        m_dispatcher = std::make_unique<EpollEventDispatcher>(m_loop.get());
    }

    void TearDown() override {
        m_dispatcher.reset();
        // ~EventLoop 在创建线程执行（wakeup channel 摘除 + fd 关闭）
        m_loop.reset();
    }

    /// @brief 未启动循环的 EventLoop（本测试线程即其 IO 线程）
    std::unique_ptr<EventLoop> m_loop;
    /// @brief 独立构造的待测 dispatcher
    std::unique_ptr<EpollEventDispatcher> m_dispatcher;

    /// @brief 注册一个可读监听到待测 dispatcher
    void RegisterRead(const std::shared_ptr<Channel>& channel) {
        channel->EnableReadEvent();                  // 置位事件集合并注册进 loop 的 epoll
        m_dispatcher->UpdateChannel(channel.get());  // 注册进待测 dispatcher（ADD 路径）
    }

    /// @brief 从待测 dispatcher 摘除（先清事件集，UpdateChannel 走内部 Remove 路径）
    void Unregister(const std::shared_ptr<Channel>& channel) {
        channel->DisableAllEvent();                  // 清事件集合并从 loop 的 epoll 摘除
        m_dispatcher->UpdateChannel(channel.get());  // 待测 dispatcher 走 Remove 路径
    }
};

/// [正] 注册可读后事件就绪即上报，OccurEvent 含 kReadable
TEST_F(EventDispatcherTest, AddAndDispatchReadable) {
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);
    auto channel = std::make_shared<Channel>(std::ref(*m_loop), fd);
    RegisterRead(channel);

    const uint64_t one = 1;
    ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));

    EventDispatcher::ChannelList active;
    m_dispatcher->Dispatcher(0, active);

    ASSERT_EQ(active.size(), 1U);
    EXPECT_EQ(active[0], channel.get());
    EXPECT_TRUE((channel->OccurEvent() & EventType::kReadable) != 0);

    Unregister(channel);
    channel.reset();
    ::close(fd);
}

/// [界] timeoutMs=0 无就绪事件立即返回空列表
TEST_F(EventDispatcherTest, TimeoutZeroReturnsEmpty) {
    EventDispatcher::ChannelList active;
    m_dispatcher->Dispatcher(0, active);
    EXPECT_TRUE(active.empty());
}

/// [界] timeoutMs>0 无事件时按超时返回（50ms 档，下界容差 45ms）
TEST_F(EventDispatcherTest, TimeoutWaitsConfigured) {
    EventDispatcher::ChannelList active;

    const auto begin = std::chrono::steady_clock::now();
    m_dispatcher->Dispatcher(50, active);
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_TRUE(active.empty());
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 45);
}

/// [正] Modify 路径：注册可读后追注册可写（EPOLL_CTL_MOD），事件集正确上报
TEST_F(EventDispatcherTest, ModifyPathReportsWritable) {
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);
    auto channel = std::make_shared<Channel>(std::ref(*m_loop), fd);
    RegisterRead(channel);

    // eventfd 计数器未满时恒可写：追注册可写触发 MOD 路径
    channel->EnableWriteEvent();
    m_dispatcher->UpdateChannel(channel.get());

    EventDispatcher::ChannelList active;
    m_dispatcher->Dispatcher(0, active);

    ASSERT_EQ(active.size(), 1U);
    EXPECT_TRUE((channel->OccurEvent() & EventType::kWritable) != 0);

    const uint64_t one = 1;
    ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    EventDispatcher::ChannelList again;
    m_dispatcher->Dispatcher(0, again);
    ASSERT_EQ(again.size(), 1U);
    EXPECT_TRUE((channel->OccurEvent() & EventType::kReadable) != 0);
    EXPECT_TRUE((channel->OccurEvent() & EventType::kWritable) != 0);

    Unregister(channel);
    channel.reset();
    ::close(fd);
}

/// [正] 移除后不再上报（UpdateChannel 的内部 Remove 路径）
TEST_F(EventDispatcherTest, RemoveStopsReporting) {
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);
    auto channel = std::make_shared<Channel>(std::ref(*m_loop), fd);
    RegisterRead(channel);

    const uint64_t one = 1;
    ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    EventDispatcher::ChannelList consume;
    m_dispatcher->Dispatcher(0, consume);
    ASSERT_EQ(consume.size(), 1U);

    Unregister(channel);

    // 再就绪也不上报
    ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    EventDispatcher::ChannelList active;
    m_dispatcher->Dispatcher(0, active);
    EXPECT_TRUE(active.empty());

    channel.reset();
    ::close(fd);
}

/// [界] 事件列表扩容：20 个源同时就绪，首轮受初始容量 16 限制，
/// 恰好返回 16 触发 resize 分支，次轮返回剩余 4
TEST_F(EventDispatcherTest, EventListGrowsBeyondInitialSize) {
    constexpr int kCount = 20;
    constexpr size_t kInitialSize = 16;
    std::vector<int> fds;
    std::vector<std::shared_ptr<Channel>> channels;
    fds.reserve(kCount);
    channels.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        ASSERT_GE(fd, 0);
        fds.push_back(fd);
        auto channel = std::make_shared<Channel>(std::ref(*m_loop), fd);
        RegisterRead(channel);
        channels.push_back(std::move(channel));
    }

    const uint64_t one = 1;
    for (int fd : fds) {
        ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    }

    EventDispatcher::ChannelList first_round;
    m_dispatcher->Dispatcher(0, first_round);
    ASSERT_EQ(first_round.size(), kInitialSize);  // 首轮恰好填满缓冲 → resize 分支

    // epoll 为水平触发且 eventfd 未读取：20 个源持续就绪；
    // 第二轮能返回 20（> 初始容量 16）即证明扩容已生效
    EventDispatcher::ChannelList second_round;
    m_dispatcher->Dispatcher(0, second_round);
    EXPECT_EQ(second_round.size(), static_cast<size_t>(kCount));

    for (auto& channel : channels) {
        Unregister(channel);
        channel.reset();
    }
    for (int fd : fds) {
        ::close(fd);
    }
}

/// [压] 1000 轮创建/注册/摘除/销毁（ADD/MOD/DEL 全路径压测）
TEST_F(EventDispatcherTest, ChannelChurnStress) {
    for (int i = 0; i < 1000; ++i) {
        const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        ASSERT_GE(fd, 0);
        auto channel = std::make_shared<Channel>(std::ref(*m_loop), fd);
        RegisterRead(channel);
        channel->EnableWriteEvent();
        m_dispatcher->UpdateChannel(channel.get());  // MOD
        Unregister(channel);
        channel.reset();
        ::close(fd);
    }
}

/// [正] 事件类型契约：EventType 与 epoll 位映射
TEST_F(EventDispatcherTest, EventTypeMappingContract) {
    EXPECT_EQ(EventType::kNone, 0);
    EXPECT_EQ(EventType::kReadable, EPOLLIN | EPOLLPRI);
    EXPECT_EQ(EventType::kWritable, EPOLLOUT);
    EXPECT_EQ(EventType::kError, EPOLLERR);
    // 事件位互不重叠
    EXPECT_EQ(EventType::kReadable & EventType::kWritable, 0);
    EXPECT_EQ(EventType::kReadable & EventType::kError, 0);
    EXPECT_EQ(EventType::kWritable & EventType::kError, 0);
}

/// [命] 析构关闭 epoll fd：100 次 EventLoop 创建/销毁进程 fd 零净增
TEST_F(EventDispatcherTest, DestructorClosesEpollFd) {
    // 释放 fixture loop：本用例在同线程反复建/毁 EventLoop，
    // one loop per thread 断言要求线程内无存活 loop（TearDown 对空指针安全）
    m_loop.reset();

    const int before = FdCount();
    ASSERT_GT(before, 0);

    for (int i = 0; i < 100; ++i) {
        EventLoop loop;
    }

    const int after = FdCount();
    EXPECT_EQ(after, before) << "epoll fd leak: before=" << before << " after=" << after;
}

}  // namespace test
}  // namespace bus
