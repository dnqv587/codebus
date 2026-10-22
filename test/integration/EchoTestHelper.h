//***********************************************
// name      : EchoTestHelper.h
// brief     : 集成测试辅助设施（手写 Echo 服务器与阻塞式客户端）
// attention : EchoServer 的 Start/Shutdown 必须在 loop 线程调用；含绕
//             源码缺陷的临时处理（TODO(D27) 标注处；D13/D18 绕行已闭环
//             回收），修复后按注释回收；EchoClient 亦被 ListenerTest
//             用作连接探针
//***********************************************
#pragma once
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/Address.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Listener.h"
#include "net/Socket.h"
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <array>
#include <atomic>

namespace bus
{
namespace test
{

/// @brief 测试用 Echo 服务器：Listener + 每连接 Channel/Socket
/// @attention Start/Shutdown 必须在 loop 线程调用
class EchoServer
{
 public:
    EchoServer(EventLoop* loop, uint16_t port)
        : m_loop(loop), m_port(port)
    {
    }

    ~EchoServer()
    {
        // Shutdown 必须在 loop 线程、loop 停止前调用
        assert(m_listener == nullptr);
        assert(m_conns.empty());
    }

    EchoServer(const EchoServer&) = delete;
    EchoServer& operator=(const EchoServer&) = delete;

    /// @brief 启动监听（loop 线程）
    void Start()
    {
        Address addr("127.0.0.1", m_port);
        m_listener = std::make_unique<Listener>(std::ref(*m_loop), addr);
        m_listener->setNewConnectionCallback([this](socket_t fd, Address) {
            OnAccept(fd);
        });
        m_listener->Listen();
    }

    /// @brief 关闭监听与所有连接（loop 线程）
    void Shutdown()
    {
        m_shutting_down = true;
        if (m_listener)
        {
            m_listener.reset();
        }
        for (auto& [fd, conn] : m_conns)
        {
            (void)fd;
            // DisableAllEvent 内部会在 Channel 处于监听状态时自动摘除
            conn->channel->DisableAllEvent();
        }
        m_conns.clear();
        // 僵尸连接在 Shutdown 时统一销毁（此时不在事件分派过程中，安全）
        m_zombies.clear();
    }

    /// @brief 已接受的连接总数
    ATTR_PURE_INLINE
    int AcceptedCount() const noexcept
    {
        return m_accepted.load();
    }

    /// @brief 已回显字节数
    ATTR_PURE_INLINE
    long long EchoedBytes() const noexcept
    {
        return m_echoed_bytes.load();
    }

 private:
    /// @brief 单连接状态
    /// @attention 输出缓冲用 string+偏移而非 Buffer：历史上为绕 D13（已闭环）
    ///            引入，现保留为实现选择，待 D27 修复重写连接生命周期时统一
    ///            现代化为 Buffer 路径
    struct Connection
    {
        /// @brief 套接字
        std::unique_ptr<Socket> socket;
        /// @brief 事件通道
        std::unique_ptr<Channel> channel;
        /// @brief 待发送数据
        std::string output;
        /// @brief 已发送偏移
        size_t sent{0};
    };

    /// @brief 接受新连接（loop 线程）
    /// @param fd 已接受的 fd
    void OnAccept(socket_t fd)
    {
        assert(m_loop->isInLoopThread());
        if (m_shutting_down)
        {
            ::close(fd);
            return;
        }
        m_accepted.fetch_add(1);

        auto conn = std::make_shared<Connection>();
        conn->socket = std::make_unique<Socket>(fd);
        conn->socket->setTcpNoDelay(true);
        conn->channel = std::make_unique<Channel>(std::ref(*m_loop), fd);

        Connection* raw = conn.get();
        conn->channel->setReadCallback([this, raw]() { HandleRead(raw); });
        conn->channel->setWriteCallback([this, raw]() { HandleWrite(raw); });
        conn->channel->setErrorCallback([this, raw]() { CloseConnection(raw); });
        conn->channel->EnableReadEvent();

        m_conns.emplace(fd, conn);
    }

    /// @brief 读事件：追加到输出缓冲并回显（loop 线程）
    /// @param conn 连接
    void HandleRead(Connection* conn)
    {
        assert(m_loop->isInLoopThread());
        std::array<char, 65536> buffer{};
        const ssize_t n = conn->socket->read(buffer.data(), buffer.size());
        if (n > 0)
        {
            conn->output.append(buffer.data(), static_cast<size_t>(n));
            m_echoed_bytes.fetch_add(n);
            HandleWrite(conn);
            return;
        }
        if (n == 0)
        {
            CloseConnection(conn);
            return;
        }
        const int err = errno;
        if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR)
        {
            CloseConnection(conn);
        }
    }

    /// @brief 写事件：尽量发送输出缓冲（loop 线程）
    /// @param conn 连接
    void HandleWrite(Connection* conn)
    {
        assert(m_loop->isInLoopThread());
        if (conn->sent >= conn->output.size())
        {
            conn->output.clear();
            conn->sent = 0;
            if (conn->channel->isListeningWrite())
            {
                conn->channel->DisableWriteEvent();
            }
            return;
        }

        const char* data = conn->output.data() + conn->sent;
        const size_t remain = conn->output.size() - conn->sent;
        const ssize_t n = conn->socket->write(data, remain);
        if (n > 0)
        {
            conn->sent += static_cast<size_t>(n);
            if (conn->sent >= conn->output.size())
            {
                conn->output.clear();
                conn->sent = 0;
                if (conn->channel->isListeningWrite())
                {
                    conn->channel->DisableWriteEvent();
                }
            }
            return;
        }
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
        {
            if (!conn->channel->isListeningWrite())
            {
                conn->channel->EnableWriteEvent();
            }
            return;
        }
        CloseConnection(conn);
    }

    /// @brief 关闭连接（loop 线程）
    /// @attention TODO(D27): 不能在回调中销毁 Channel：EventLoop 的活动
    ///            Channel 列表不保护对象存活，同一轮分派中后续事件会访问
    ///            已释放内存（TSAN 已复现 heap-use-after-free，
    ///            src/net/EventLoop.cc:87-98）。因此转移到僵尸列表，待
    ///            Shutdown（事件分派之外）统一销毁；EventLoop 采用
    ///            tie/weak_ptr 或延迟销毁协议修复后删除僵尸列表直接销毁。
    /// @param conn 连接
    void CloseConnection(Connection* conn)
    {
        assert(m_loop->isInLoopThread());
        // DisableAllEvent 内部会在 Channel 处于监听状态时自动摘除
        conn->channel->DisableAllEvent();

        auto iter = m_conns.find(conn->socket->fd());
        if (iter != m_conns.end())
        {
            m_zombies.push_back(std::move(iter->second));
            m_conns.erase(iter);
        }
    }

    /// @brief 所属事件循环
    EventLoop* m_loop;
    /// @brief 监听端口
    uint16_t m_port;
    /// @brief 监听器
    std::unique_ptr<Listener> m_listener;
    /// @brief 活跃连接表（key: fd）
    std::unordered_map<socket_t, std::shared_ptr<Connection>> m_conns;
    /// @brief 已关闭但待销毁的连接（TODO(D27): 避免在事件分派中析构 Channel）
    std::vector<std::shared_ptr<Connection>> m_zombies;
    /// @brief 已接受连接计数
    std::atomic<int> m_accepted{0};
    /// @brief 已回显字节计数
    std::atomic<long long> m_echoed_bytes{0};
    /// @brief 是否正在关闭（仅 loop 线程访问）
    bool m_shutting_down{false};
};

/// @brief 阻塞式 Echo 客户端
class EchoClient
{
 public:
    /// @brief 连接服务器
    /// @param port 服务器端口
    /// @param timeoutMs 收发超时
    explicit EchoClient(uint16_t port, int timeoutMs = 5000)
    {
        m_fd.reset(::socket(AF_INET, SOCK_STREAM, 0));
        if (!m_fd.valid())
        {
            return;
        }

        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ::setsockopt(m_fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(m_fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(m_fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            m_fd.reset();
        }
    }

    /// @brief 是否连接成功
    ATTR_PURE_INLINE
    bool valid() const noexcept
    {
        return m_fd.valid();
    }

    /// @brief 发送全部数据
    /// @param data 数据
    /// @param len 长度
    /// @return 全部发送成功返回 true
    bool SendAll(const void* data, size_t len)
    {
        const char* pos = static_cast<const char*>(data);
        size_t remain = len;
        while (remain > 0)
        {
            const ssize_t n = ::send(m_fd.get(), pos, remain, MSG_NOSIGNAL);
            if (n > 0)
            {
                pos += n;
                remain -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            return false;
        }
        return true;
    }

    /// @brief 接收指定长度数据
    /// @param data 输出缓冲
    /// @param len 期望长度
    /// @return 完整收到返回 true，超时/对端关闭返回 false
    bool RecvExact(void* data, size_t len)
    {
        char* pos = static_cast<char*>(data);
        size_t remain = len;
        while (remain > 0)
        {
            const ssize_t n = ::recv(m_fd.get(), pos, remain, 0);
            if (n > 0)
            {
                pos += n;
                remain -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            return false;
        }
        return true;
    }

    /// @brief 关闭写方向
    void ShutdownWrite()
    {
        ::shutdown(m_fd.get(), SHUT_WR);
    }

    /// @brief 获取 fd
    ATTR_PURE_INLINE
    int fd() const noexcept
    {
        return m_fd.get();
    }

 private:
    /// @brief 客户端 fd
    ScopedFd m_fd;
};

} // namespace test
} // namespace bus
