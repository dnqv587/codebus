//***********************************************
// name      : ChannelTest.cc
// brief     : net 模块单元测试：被测 src/net/Channel.h/.cc（fd 事件注册与回调分派）
// attention : Channel 的构造、Enable/Disable、销毁必须在 loop 线程执行；
//             D27 已闭环（tie 守卫）：DestroyDuringDispatch/
//             TieExpiredEventDropped 覆盖分派栈内销毁与宿主已亡作废路径
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/Channel.h"
#include "net/EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bus
{
namespace test
{

class ChannelTest : public ::testing::Test
{
 protected:
    void SetUp() override
    {
        m_loop_thread = std::make_unique<LoopThread>();
    }

    void TearDown() override
    {
        m_loop_thread.reset();
    }

    /// @brief 运行事件循环的线程
    std::unique_ptr<LoopThread> m_loop_thread;
};

TEST_F(ChannelTest, DispatchAllCallbacks)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);

    int read_count = 0;
    int write_count = 0;
    int error_count = 0;
    RunInLoopSync(loop, [&]() {
        Channel channel(std::ref(*loop), fd);
        channel.setReadCallback([&]() { ++read_count; });
        channel.setWriteCallback([&]() { ++write_count; });
        channel.setErrorCallback([&]() { ++error_count; });

        channel.setOccurEvent(EventType::kReadable);
        channel.HandleEvent();
        channel.setOccurEvent(EventType::kWritable);
        channel.HandleEvent();
        channel.setOccurEvent(EventType::kError);
        channel.HandleEvent();
        channel.setOccurEvent(EventType::kReadable | EventType::kWritable | EventType::kError);
        channel.HandleEvent();
    });

    EXPECT_EQ(read_count, 2);
    EXPECT_EQ(write_count, 2);
    EXPECT_EQ(error_count, 2);
    ::close(fd);
}

TEST_F(ChannelTest, DispatchWithoutCallbacks)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);

    RunInLoopSync(loop, [&]() {
        Channel channel(std::ref(*loop), fd);
        channel.setOccurEvent(EventType::kReadable | EventType::kWritable | EventType::kError);
        channel.HandleEvent(); // 未设置回调，不应崩溃
    });

    ::close(fd);
}

TEST_F(ChannelTest, EnableDisableEvents)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);

    RunInLoopSync(loop, [&]() {
        Channel channel(std::ref(*loop), fd);
        EXPECT_EQ(channel.ListeningEvent(), EventType::kNone);

        channel.EnableReadEvent();
        EXPECT_EQ(channel.ListeningEvent(), EventType::kReadable);
        EXPECT_TRUE(channel.isListeningRead());
        EXPECT_FALSE(channel.isListeningWrite());

        channel.EnableWriteEvent();
        EXPECT_EQ(channel.ListeningEvent(), EventType::kReadable | EventType::kWritable);
        EXPECT_TRUE(channel.isListeningWrite());

        channel.DisableReadEvent();
        EXPECT_EQ(channel.ListeningEvent(), EventType::kWritable);
        EXPECT_FALSE(channel.isListeningRead());

        channel.DisableWriteEvent();
        EXPECT_EQ(channel.ListeningEvent(), EventType::kNone);

        channel.EnableReadEvent();
        channel.DisableAllEvent();
        EXPECT_EQ(channel.ListeningEvent(), EventType::kNone);
    });

    ::close(fd);
}

TEST_F(ChannelTest, RemoveChannelInCallback)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);

    std::atomic<int> calls{0};
    std::atomic<bool> removed{false};
    std::shared_ptr<Channel> channel;
    RunInLoopSync(loop, [&]() {
        channel = std::make_shared<Channel>(std::ref(*loop), fd);
        channel->setReadCallback([&, raw = channel.get()]() {
            uint64_t value = 0;
            (void)::read(raw->fd(), &value, sizeof(value));
            calls.fetch_add(1);
            raw->DisableAllEvent();
            removed.store(true);
        });
        channel->EnableReadEvent();
    });

    const uint64_t one = 1;
    ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    ASSERT_TRUE(WaitFor([&]() { return calls.load() == 1; }));

    RunInLoopSync(loop, [&]() {
        if (!removed.load())
        {
            channel->DisableAllEvent();
        }
        channel.reset();
    });

    ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(calls.load(), 1); // 已移除，不再触发
    ::close(fd);
}

TEST_F(ChannelTest, UpdateRemoveStress)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);

    RunInLoopSync(loop, [&]() {
        for (int i = 0; i < 1000; ++i)
        {
            Channel channel(std::ref(*loop), fd);
            channel.EnableReadEvent();
            channel.DisableAllEvent();
        }
    });

    ::close(fd);
}

TEST_F(ChannelTest, ManyChannelsConcurrentEvents)
{
    constexpr int kChannelCount = 128;
    constexpr int kWriterCount = 4;
    constexpr int kWritesPerWriter = 1000;

    struct ChannelState
    {
        int fd = -1;
        std::shared_ptr<Channel> channel;
    };

    EventLoop* loop = m_loop_thread->loop();
    auto states = std::make_shared<std::vector<std::unique_ptr<ChannelState>>>();
    auto total = std::make_shared<std::atomic<uint64_t>>(0);

    RunInLoopSync(loop, [&]() {
        states->reserve(kChannelCount);
        for (int i = 0; i < kChannelCount; ++i)
        {
            auto state = std::make_unique<ChannelState>();
            state->fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            EXPECT_GE(state->fd, 0);
            state->channel = std::make_shared<Channel>(std::ref(*loop), state->fd);
            ChannelState* raw = state.get();
            state->channel->setReadCallback([raw, total]() {
                uint64_t value = 0;
                const ssize_t n = ::read(raw->fd, &value, sizeof(value));
                if (n == static_cast<ssize_t>(sizeof(value)))
                {
                    total->fetch_add(value);
                }
            });
            state->channel->EnableReadEvent();
            states->push_back(std::move(state));
        }
    });

    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (int writer = 0; writer < kWriterCount; ++writer)
    {
        writers.emplace_back([writer, &states]() {
            const uint64_t one = 1;
            for (int i = 0; i < kWritesPerWriter; ++i)
            {
                const size_t index =
                    static_cast<size_t>((writer * kWritesPerWriter + i) % states->size());
                const ssize_t n = ::write((*states)[index]->fd, &one, sizeof(one));
                ASSERT_EQ(n, static_cast<ssize_t>(sizeof(one)));
            }
        });
    }
    for (auto& thread : writers)
    {
        thread.join();
    }

    constexpr uint64_t kExpected = static_cast<uint64_t>(kWriterCount) * kWritesPerWriter;
    const bool all_received = WaitFor([&]() { return total->load() == kExpected; });

    // 先清理后断言：断言失败也必须摘除并销毁 Channel（否则 loop 线程将
    // 分派悬垂指针，把断言失败升级为进程级 UAF）
    RunInLoopSync(loop, [&]() {
        for (auto& state : *states)
        {
            state->channel->DisableAllEvent();
            state->channel.reset();
            ::close(state->fd);
        }
        states->clear();
    });

    EXPECT_TRUE(all_received) << "received=" << total->load() << " expected=" << kExpected;
}

// ---------------------------------------------------------------------------
// 新增场景（P2）
// ---------------------------------------------------------------------------

/// [界] getter 契约：fd()/OwnerLoop()/OccurEvent() 初始值
TEST_F(ChannelTest, GetterContract)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);

    RunInLoopSync(loop, [&]() {
        Channel channel(std::ref(*loop), fd);
        EXPECT_EQ(channel.fd(), fd);
        EXPECT_EQ(channel.OwnerLoop(), loop);
        EXPECT_EQ(channel.OccurEvent(), EventType::kNone);
        EXPECT_EQ(channel.ListeningEvent(), EventType::kNone);

        channel.setOccurEvent(EventType::kReadable);
        EXPECT_EQ(channel.OccurEvent(), EventType::kReadable);
    });

    ::close(fd);
}

/// [界] DisableAll 后复用同一 Channel 再注册事件（对象可复用性）
TEST_F(ChannelTest, ReEnableAfterDisableAll)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);

    std::atomic<int> calls{0};
    std::shared_ptr<Channel> channel;
    RunInLoopSync(loop, [&]() {
        channel = std::make_shared<Channel>(std::ref(*loop), fd);
        channel->EnableReadEvent();
        channel->DisableAllEvent();
        EXPECT_EQ(channel->ListeningEvent(), EventType::kNone);

        channel->setReadCallback([&]() {
            uint64_t value = 0;
            (void)::read(fd, &value, sizeof(value));
            calls.fetch_add(1);
        });
        channel->EnableReadEvent(); // 复用同一 Channel 再注册
        EXPECT_EQ(channel->ListeningEvent(), EventType::kReadable);
    });

    const uint64_t one = 1;
    ASSERT_EQ(::write(fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    EXPECT_TRUE(WaitFor([&]() { return calls.load() == 1; }));

    RunInLoopSync(loop, [&]() {
        channel->DisableAllEvent();
        channel.reset();
    });
    ::close(fd);
}

/// [并] D27：宿主在自身分派栈内被销毁——tie 守卫保证析构推迟到 HandleEvent
///      返回之后（TcpServer removeConnection / EchoServer CloseConnection 的
///      真实路径），分派循环的裸指针访问安全
TEST_F(ChannelTest, DestroyDuringDispatch)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd_a = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    const int fd_b = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd_a, 0);
    ASSERT_GE(fd_b, 0);

    // 宿主形态：宿主持有 Channel 并 tie 自身（模拟 TCPConnection 与连接表）
    struct Host
    {
        explicit Host(std::unique_ptr<Channel> ch) : m_channel(std::move(ch)) {}
        ~Host() { m_destroyed->store(true); }
        std::unique_ptr<Channel> m_channel;
        std::shared_ptr<std::atomic<bool>> m_destroyed =
            std::make_shared<std::atomic<bool>>(false);
    };

    std::unordered_map<int, std::shared_ptr<Host>> conns;   // 模拟连接表（仅 loop 线程访问）
    std::atomic<bool> victim_dispatched{false};
    std::atomic<bool> host_alive_in_callback{false};

    RunInLoopSync(loop, [&]() {
        auto host_a = std::make_shared<Host>(std::make_unique<Channel>(std::ref(*loop), fd_a));
        auto host_b = std::make_shared<Host>(std::make_unique<Channel>(std::ref(*loop), fd_b));
        host_a->m_channel->tie(host_a);
        host_b->m_channel->tie(host_b);

        host_a->m_channel->setReadCallback([&]() {
            uint64_t value = 0;
            (void)::read(fd_a, &value, sizeof(value));
        });
        host_b->m_channel->setReadCallback(
            [channel_b = host_b->m_channel.get(), destroyed = host_b->m_destroyed,
             &victim_dispatched, &host_alive_in_callback, &conns, fd_b]() {
                uint64_t value = 0;
                (void)::read(fd_b, &value, sizeof(value));
                // 关闭链（模拟 handleClose → close_cb → 连接表 erase）：
                // 守卫应保证 Host 在 HandleEvent 返回前不析构
                host_alive_in_callback.store(!destroyed->load());
                channel_b->DisableAllEvent();
                conns.erase(fd_b);
                victim_dispatched.store(true);   // erase 完成后再发完成信号（同步次序）
            });
        host_a->m_channel->EnableReadEvent();
        host_b->m_channel->EnableReadEvent();
        conns.emplace(fd_a, std::move(host_a));
        conns.emplace(fd_b, std::move(host_b));
    });

    // 两个 fd 同时就绪 → 同一轮 epoll 分派；B 的回调内从连接表销毁自身
    const uint64_t one = 1;
    ASSERT_EQ(::write(fd_a, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    ASSERT_EQ(::write(fd_b, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));

    EXPECT_TRUE(WaitFor([&]() { return victim_dispatched.load(); }));
    EXPECT_TRUE(host_alive_in_callback.load());    // 守卫生效：回调内宿主未析构
    EXPECT_EQ(conns.find(fd_b), conns.end());      // 已从连接表移除

    RunInLoopSync(loop, [&]() {
        if (auto iter = conns.find(fd_a); iter != conns.end())
        {
            iter->second->m_channel->DisableAllEvent();
            conns.erase(iter);
        }
    });
    ::close(fd_a);
    ::close(fd_b);
}

/// [界] D27：tie 宿主已亡（anchor 过期）——事件作废，回调不再执行（防御分支）
TEST_F(ChannelTest, TieExpiredEventDropped)
{
    EventLoop* loop = m_loop_thread->loop();
    const int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(fd, 0);

    std::atomic<int> calls{0};
    RunInLoopSync(loop, [&]() {
        Channel channel(std::ref(*loop), fd);
        auto anchor = std::make_shared<int>(0);
        channel.tie(anchor);
        channel.setReadCallback([&]() { calls.fetch_add(1); });

        anchor.reset();   // 宿主亡（Channel 独立持有，构造守卫失效形态）
        channel.setOccurEvent(EventType::kReadable);
        channel.HandleEvent();   // 作废：不回调
        EXPECT_EQ(calls.load(), 0);
    });

    ::close(fd);
}

/// [压] 多 writer 并发写加深：8 writer × 64 channel × 500 写（总事件量与 4×128×1000 相当）
TEST_F(ChannelTest, ManyChannelsHighWriteConcurrency)
{
    constexpr int kChannelCount = 64;
    constexpr int kWriterCount = 8;
    constexpr int kWritesPerWriter = 500;

    struct ChannelState
    {
        int fd = -1;
        std::shared_ptr<Channel> channel;
    };

    EventLoop* loop = m_loop_thread->loop();
    auto states = std::make_shared<std::vector<std::unique_ptr<ChannelState>>>();
    auto total = std::make_shared<std::atomic<uint64_t>>(0);

    RunInLoopSync(loop, [&]() {
        states->reserve(kChannelCount);
        for (int i = 0; i < kChannelCount; ++i)
        {
            auto state = std::make_unique<ChannelState>();
            state->fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            EXPECT_GE(state->fd, 0);
            state->channel = std::make_shared<Channel>(std::ref(*loop), state->fd);
            ChannelState* raw = state.get();
            state->channel->setReadCallback([raw, total]() {
                uint64_t value = 0;
                const ssize_t n = ::read(raw->fd, &value, sizeof(value));
                if (n == static_cast<ssize_t>(sizeof(value)))
                {
                    total->fetch_add(value);
                }
            });
            state->channel->EnableReadEvent();
            states->push_back(std::move(state));
        }
    });

    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (int writer = 0; writer < kWriterCount; ++writer)
    {
        writers.emplace_back([writer, &states]() {
            const uint64_t one = 1;
            for (int i = 0; i < kWritesPerWriter; ++i)
            {
                const size_t index =
                    static_cast<size_t>((writer * kWritesPerWriter + i) % states->size());
                const ssize_t n = ::write((*states)[index]->fd, &one, sizeof(one));
                ASSERT_EQ(n, static_cast<ssize_t>(sizeof(one)));
            }
        });
    }
    for (auto& thread : writers)
    {
        thread.join();
    }

    constexpr uint64_t kExpected = static_cast<uint64_t>(kWriterCount) * kWritesPerWriter;
    const bool all_received = WaitFor([&]() { return total->load() == kExpected; });

    // 先清理后断言（同 ManyChannelsConcurrentEvents 的失败路径保护）
    RunInLoopSync(loop, [&]() {
        for (auto& state : *states)
        {
            state->channel->DisableAllEvent();
            state->channel.reset();
            ::close(state->fd);
        }
        states->clear();
    });

    EXPECT_TRUE(all_received) << "received=" << total->load() << " expected=" << kExpected;
}

} // namespace test
} // namespace bus
