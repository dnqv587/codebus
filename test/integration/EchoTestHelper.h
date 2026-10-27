//***********************************************
// name      : EchoTestHelper.h
// brief     : 集成测试辅助设施（TcpServer 装配的 Echo 服务器与阻塞式客户端）
// attention : EchoServer 的 Shutdown 在 loop 线程调用为同步完成（单 loop 模式）；
//             历史手拼 Channel/Socket 与 D13/D18/D27 绕行已随 TcpServer 迁移
//             退役；EchoClient 亦被 ListenerTest 用作连接探针
//***********************************************
#pragma once
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/EventLoop.h"
#include "net/TCPConnection.h"
#include "net/TcpServer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bus {
namespace test {

/// @brief 测试用 Echo 服务器：TcpServer 装配（单 loop）
/// @attention Shutdown 在 loop 线程调用为同步完成（Stop 的执行链全在
///            本线程；跨线程调用则异步完成）
class EchoServer {
 public:
    /// @brief 构造并装配 echo 回调（不启动）
    /// @param loop 服务器 loop（单 loop 模式）
    /// @param port 监听端口
    EchoServer(EventLoop* loop, uint16_t port)
        : m_server(std::make_unique<TcpServer>(loop, "127.0.0.1:" + std::to_string(port), 0)) {
        m_server->setConnectionCallback([this](const TCPConnection::ptr& conn) {
            if (conn->isConnected()) {
                m_accepted.fetch_add(1);
            }
        });
        m_server->setMessageCallback([this](const TCPConnection::ptr& conn, Buffer* buf) {
            m_echoed_bytes.fetch_add(buf->ReadableSize());
            conn->send(buf->PeekAll());  // 回显（loop 线程内同步入队）
            buf->SkipRead(buf->ReadableSize());
        });
    }

    ~EchoServer() = default;  // TcpServer 析构契约：必须已 Shutdown（否则其断言触发）

    EchoServer(const EchoServer&) = delete;
    EchoServer& operator=(const EchoServer&) = delete;

    /// @brief 启动监听（异步装配；建客户端前建议经启动屏障，见 TcpServerTest）
    void Start() {
        m_server->Start();
    }

    /// @brief 全停：摘监听 → 逐连接关闭（loop 线程调用为同步完成）
    void Shutdown() {
        m_server->Stop();
    }

    /// @brief 已建立连接总数（conn_cb 语义：建连即计）
    ATTR_PURE_INLINE
    int AcceptedCount() const noexcept {
        return m_accepted.load();
    }

    /// @brief 已回显字节数（msg_cb 语义：到达即计）
    ATTR_PURE_INLINE
    long long EchoedBytes() const noexcept {
        return m_echoed_bytes.load();
    }

 private:
    /// @brief 被装配的 TCP 服务器
    std::unique_ptr<TcpServer> m_server;
    /// @brief 已建立连接计数
    std::atomic<int> m_accepted{0};
    /// @brief 已回显字节计数
    std::atomic<long long> m_echoed_bytes{0};
};

/// @brief 阻塞式 Echo 客户端
class EchoClient {
 public:
    /// @brief 连接服务器
    /// @param port 服务器端口
    /// @param timeoutMs 收发超时
    explicit EchoClient(uint16_t port, int timeoutMs = 5000) {
        m_fd.reset(::socket(AF_INET, SOCK_STREAM, 0));
        if (!m_fd.valid()) {
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
        if (::connect(m_fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            m_fd.reset();
        }
    }

    /// @brief 是否连接成功
    ATTR_PURE_INLINE
    bool valid() const noexcept {
        return m_fd.valid();
    }

    /// @brief 发送全部数据
    /// @param data 数据
    /// @param len 长度
    /// @return 全部发送成功返回 true
    bool SendAll(const void* data, size_t len) {
        const char* pos = static_cast<const char*>(data);
        size_t remain = len;
        while (remain > 0) {
            const ssize_t n = ::send(m_fd.get(), pos, remain, MSG_NOSIGNAL);
            if (n > 0) {
                pos += n;
                remain -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
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
    bool RecvExact(void* data, size_t len) {
        char* pos = static_cast<char*>(data);
        size_t remain = len;
        while (remain > 0) {
            const ssize_t n = ::recv(m_fd.get(), pos, remain, 0);
            if (n > 0) {
                pos += n;
                remain -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    /// @brief 关闭写方向
    void ShutdownWrite() {
        ::shutdown(m_fd.get(), SHUT_WR);
    }

    /// @brief 获取 fd
    ATTR_PURE_INLINE
    int fd() const noexcept {
        return m_fd.get();
    }

 private:
    /// @brief 客户端 fd
    ScopedFd m_fd;
};

}  // namespace test
}  // namespace bus
