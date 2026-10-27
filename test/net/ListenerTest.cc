//***********************************************
// name      : ListenerTest.cc
// brief     : net 模块单元测试：被测 src/net/Listener.h/.cc（accept 监听器回调与 fd 管理）
// attention : Listener 的构造/Listen/销毁必须在 loop 线程执行
//***********************************************
#include <gtest/gtest.h>
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "Exception.hpp"
#include "integration/EchoTestHelper.h"
#include "net/Address.h"
#include "net/EventLoop.h"
#include "net/Listener.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>

namespace bus {
namespace test {

class ListenerTest : public ::testing::Test {
 protected:
    void SetUp() override { m_loop_thread = std::make_unique<LoopThread>(); }

    void TearDown() override { m_loop_thread.reset(); }

    /// @brief 运行事件循环的线程
    std::unique_ptr<LoopThread> m_loop_thread;
};

TEST_F(ListenerTest, AcceptSingleConnection) {
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::mutex mutex;
    std::vector<std::pair<socket_t, Address>> accepted;

    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address address) {
            std::lock_guard<std::mutex> lock(mutex);
            accepted.emplace_back(fd, std::move(address));
        });
        listener->Listen();
    });

    EchoClient client(port);
    ASSERT_TRUE(client.valid());

    ASSERT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted.size() == 1;
    }));

    {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_EQ(accepted[0].second.ipString(), "127.0.0.1");
        EXPECT_GT(accepted[0].second.port(), 0);
        EXPECT_GT(accepted[0].first, 0);
    }

    RunInLoopSync(loop, [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [fd, address] : accepted) {
            (void)address;
            ::close(fd);
        }
        accepted.clear();
        delete listener;
    });
}

TEST_F(ListenerTest, ConcurrentAccept) {
    constexpr int kClientThreads = 8;
    constexpr int kClientsPerThread = 8;
    constexpr int kExpected = kClientThreads * kClientsPerThread;

    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::mutex mutex;
    std::vector<socket_t> accepted;
    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address) {
            std::lock_guard<std::mutex> lock(mutex);
            accepted.push_back(fd);
        });
        listener->Listen();
    });

    std::vector<std::thread> clients;
    clients.reserve(kClientThreads);
    for (int t = 0; t < kClientThreads; ++t) {
        clients.emplace_back([port]() {
            for (int i = 0; i < kClientsPerThread; ++i) {
                EchoClient client(port, 3000);
                ASSERT_TRUE(client.valid());
            }
        });
    }
    for (auto& thread : clients) {
        thread.join();
    }

    ASSERT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted.size() == static_cast<size_t>(kExpected);
    })) << "accepted="
        << accepted.size();

    RunInLoopSync(loop, [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        // 校验 fd 唯一
        std::vector<socket_t> sorted = accepted;
        std::sort(sorted.begin(), sorted.end());
        EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end());

        for (socket_t fd : accepted) {
            ::close(fd);
        }
        accepted.clear();
        delete listener;
    });
}

/// [命] 析构释放 fd：50 次创建/Listen/销毁 fd 零净增（Debug 下安全销毁）
TEST_F(ListenerTest, DestructorReleasesFd) {
    EventLoop* loop = m_loop_thread->loop();
    const int before = RunInLoopSync(loop, []() { return FdCount(); });
    ASSERT_GT(before, 0);

    RunInLoopSync(loop, [loop]() {
        for (int i = 0; i < 50; ++i) {
            const uint16_t port = GetFreePort();
            ASSERT_GT(port, 0);
            Listener listener(std::ref(*loop), Address("127.0.0.1", port));
            listener.Listen();
        }
    });

    const int after = RunInLoopSync(loop, []() { return FdCount(); });
    EXPECT_LE(after, before + 1) << "before=" << before << " after=" << after;
}

TEST_F(ListenerTest, NoCallbackClosesAcceptedConnection) {
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->Listen();  // 不设置新连接回调
    });

    EchoClient client(port, 1000);
    ASSERT_TRUE(client.valid());

    char byte = 0;
    // 服务器未处理连接时会主动关闭，客户端读取应为对端关闭或超时失败
    EXPECT_FALSE(client.RecvExact(&byte, 1));

    RunInLoopSync(loop, [listener]() { delete listener; });
}

// ---------------------------------------------------------------------------
// 新增场景（P4）
// ---------------------------------------------------------------------------

/// [压] accept 风暴加深：16 线程 × 16 客户端 + fd 唯一性校验
TEST_F(ListenerTest, AcceptStorm16x16) {
    constexpr int kClientThreads = 16;
    constexpr int kClientsPerThread = 16;
    constexpr int kExpected = kClientThreads * kClientsPerThread;

    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::mutex mutex;
    std::vector<socket_t> accepted;
    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address) {
            std::lock_guard<std::mutex> lock(mutex);
            accepted.push_back(fd);
        });
        listener->Listen();
    });

    std::vector<std::thread> clients;
    clients.reserve(kClientThreads);
    for (int t = 0; t < kClientThreads; ++t) {
        clients.emplace_back([port]() {
            for (int i = 0; i < kClientsPerThread; ++i) {
                EchoClient client(port, 5000);
                ASSERT_TRUE(client.valid());
            }
        });
    }
    for (auto& thread : clients) {
        thread.join();
    }

    ASSERT_TRUE(WaitFor(
        [&]() {
            std::lock_guard<std::mutex> lock(mutex);
            return accepted.size() == static_cast<size_t>(kExpected);
        },
        std::chrono::milliseconds(10000)))
        << "accepted=" << accepted.size();

    RunInLoopSync(loop, [&]() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<socket_t> sorted = accepted;
        std::sort(sorted.begin(), sorted.end());
        EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end());

        for (socket_t fd : accepted) {
            ::close(fd);
        }
        accepted.clear();
        delete listener;
    });
}

// TODO(Q14 残留): Listen() 二次调用走 listen()+EnableReadEvent() 幂等路径，
// 不报错也不影响后续 accept（Q14 的 bind 上抛已闭环（I1），本条为 Listen
// 重复调用的宽松语义——维持锁定，若确认为应显式拒绝/告警再翻转）
TEST_F(ListenerTest, ListenTwiceIsHarmless) {
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::mutex mutex;
    int accepted_count = 0;
    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address) {
            std::lock_guard<std::mutex> lock(mutex);
            ++accepted_count;
            ::close(fd);
        });
        listener->Listen();
        listener->Listen();  // 二次调用
    });

    EchoClient client(port);
    ASSERT_TRUE(client.valid());
    EXPECT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted_count == 1;
    }));

    RunInLoopSync(loop, [listener]() { delete listener; });
}

/// [错] Q14 已闭环（I1）：端口被占用时第二个 Listener 构造即抛
///      SocketError——bind 失败不再静默（幽灵启动无 API 可感知的旧状已修）
TEST_F(ListenerTest, BindOccupiedPortThrows) {
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    Listener* first = nullptr;
    RunInLoopSync(loop, [&]() {
        first = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        first->Listen();

        EXPECT_THROW(
            {
                Listener second(std::ref(*loop), Address("127.0.0.1", port));
                (void)second;
            },
            Exception::SocketError);
    });

    // 第一个 Listener 仍正常工作
    std::mutex mutex;
    int accepted_count = 0;
    RunInLoopSync(loop, [&]() {
        first->setNewConnectionCallback([&](socket_t fd, Address) {
            std::lock_guard<std::mutex> lock(mutex);
            ++accepted_count;
            ::close(fd);
        });
    });

    EchoClient client(port, 1000);
    ASSERT_TRUE(client.valid());
    EXPECT_TRUE(WaitFor([&]() {
        std::lock_guard<std::mutex> lock(mutex);
        return accepted_count == 1;
    }));

    RunInLoopSync(loop, [first]() { delete first; });
}

// ---------------------------------------------------------------------------
// 新增场景（P5：accept 循环 + idleFd EMFILE 拒绝）
// ---------------------------------------------------------------------------

namespace {

/// @brief 统计当前进程打开的 fd 数（/proc/self/fd 非点目录项，扣除统计用目录 fd）
size_t CountOpenFds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return 0;
    }
    size_t count = 0;
    while (const dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    ::closedir(dir);
    return count - 1;  // closedir 后统计用目录 fd 已释放（计数时它在册）
}

}  // namespace

/// [界] EMFILE 拒绝路径：收缩 NOFILE 软限至无空闲名额——accept 得 EMFILE，
///      idleFd 释放名额确定性拒绝积压连接（accepted 计数不增）；恢复软限后
///      listener 继续正常接受（机制未破坏监听）
TEST_F(ListenerTest, EmfileReleasesIdleSlotAndRecovers) {
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    // 软限收缩守卫：任何路径（含断言失败）恢复原值
    rlimit original{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
    struct RlimitGuard {
        rlimit limit;
        ~RlimitGuard() { ::setrlimit(RLIMIT_NOFILE, &limit); }
    } guard{original};

    std::atomic<int> accepted{0};
    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address) {
            accepted.fetch_add(1);
            ::close(fd);  // 立即关闭：只关心计数，不持有连接 fd
        });
        listener->Listen();
    });

    {
        EchoClient warmup(port);
        ASSERT_TRUE(warmup.valid());
        ASSERT_TRUE(WaitFor([&]() { return accepted.load() == 1; }));
    }

    bool emfile_held = false;
    for (int margin = 1; margin <= 3 && !emfile_held; ++margin) {
        // 预建客户端 socket（收缩前分配 fd）：收缩后 connect 不需新 fd，
        // SYN 入 backlog；服务器 accept 因无空闲名额得 EMFILE
        const int pending_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        ASSERT_GT(pending_fd, 0);

        const size_t counted = CountOpenFds();  // 含统计目录 fd 自身（+1）
        rlimit shrunk = original;
        shrunk.rlim_cur = counted - static_cast<size_t>(margin);  // 逐次收紧
        if (shrunk.rlim_cur >= original.rlim_cur) {
            ::close(pending_fd);
            break;  // 环境余量异常，放弃收缩（下方按未触发处理）
        }
        ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &shrunk), 0) << "rlim_cur=" << shrunk.rlim_cur << " counted=" << counted;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)::connect(pending_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

        const int before = accepted.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        emfile_held = (accepted.load() == before);  // 积压未被正常接受

        // 恢复软限并释放本轮 pending fd
        ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &original), 0);
        ::close(pending_fd);
    }
    EXPECT_TRUE(emfile_held) << "EMFILE 路径未触发（环境差异）";

    // 恢复后监听不受损：新连接正常接受
    {
        EchoClient after(port);
        ASSERT_TRUE(after.valid());
        ASSERT_TRUE(WaitFor([&]() { return accepted.load() >= 2; }));
    }

    RunInLoopSync(loop, [listener]() { delete listener; });
}

/// [正] C1 配套：Listener::Stop 摘事件 + 拒绝新连接（shutdown 语义）；
///      对象与 fd 不销毁（留待宿主析构）；二次 Stop 幂等
TEST_F(ListenerTest, StopRefusesNewConnections) {
    EventLoop* loop = m_loop_thread->loop();
    const uint16_t port = GetFreePort();
    ASSERT_GT(port, 0);

    std::atomic<int> accepted{0};
    Listener* listener = nullptr;
    RunInLoopSync(loop, [&]() {
        listener = new Listener(std::ref(*loop), Address("127.0.0.1", port));
        listener->setNewConnectionCallback([&](socket_t fd, Address) {
            accepted.fetch_add(1);
            ::close(fd);
        });
        listener->Listen();
    });

    {
        EchoClient warmup(port);
        ASSERT_TRUE(warmup.valid());
        ASSERT_TRUE(WaitFor([&]() { return accepted.load() == 1; }));
    }

    RunInLoopSync(loop, [&]() {
        listener->Stop();
        listener->Stop();  // 幂等
    });
    EXPECT_FALSE(EchoClient(port, 1000).valid()) << "Stop 后新连接应被拒";

    RunInLoopSync(loop, [listener]() { delete listener; });  // fd 在析构关闭
}

}  // namespace test
}  // namespace bus
