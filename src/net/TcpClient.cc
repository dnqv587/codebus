//***********************************************
// name      : TcpClient.cc
// brief     : TCP 客户端实现（Connector：非阻塞连接与退避重试）
// attention : None
//***********************************************
#include "net/TcpClient.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstring>

#include "log/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "Util.h"

namespace bus
{

static Logger* g_log = GET_LOGGER("sys");

namespace
{

/// @brief errno 是否为可退避重试的临时连接错误（muduo Connector 同款分类）
/// @param err errno 值
/// @return true=临时错误（可重试）
bool IsTemporaryConnectError(int err) noexcept
{
    return err == EAGAIN || err == EADDRINUSE || err == EADDRNOTAVAIL ||
           err == ECONNREFUSED || err == ENETUNREACH || err == ENOTCONN;
}

} // namespace

Connector::Connector(View<EventLoop> loop, const Address& serverAddr)
    : m_loop(loop),
      m_server_addr(serverAddr)
{
}

Connector::~Connector()
{
    // 契约：析构前必须 Stop 完成收尾（fd 已移交/关闭、定时器宿主已销毁、
    // Channel 已摘除注册——channel 回调捕裸 this，未摘除即销毁将悬垂）。
    // release 同样强制（审查 #5）：违约立即中止，而非静默退化为 UAF
    if (m_fd >= 0 || m_timer_manager != nullptr ||
        (m_channel && m_channel->ListeningEvent() != EventType::kNone))
    {
        LOG_FATAL(g_log) << "Connector contract violated: fd=" << m_fd
                         << ", timer_manager=" << (m_timer_manager != nullptr)
                         << ", channel registered"
                         << ((m_channel && m_channel->ListeningEvent() != EventType::kNone)
                                 ? " yes"
                                 : " no");
        SystemUtil::Abort();
    }
}

void Connector::Start()
{
    bool expected = false;
    if (!m_connect.compare_exchange_strong(expected, true))
    {
        // 审查 #2：重复 Start（含未启用 retry 时连接断开后的再次调用）非静默——
        // 非 retry 连接器为单次生命周期（Stop 前不可再发起）
        LOG_WARN(g_log) << "Connector::Start ignored: already started or spent"
                        << " (non-retry connector is single-shot; enableRetry or recreate)";
        return;
    }
    m_loop.Peer().RunInLoop([self = shared_from_this()]() { self->startInLoop(); });
}

void Connector::Restart()
{
    assert(m_loop.Peer().isInLoopThread());
    if (m_state == kConnecting)
    {
        // 审查 #4：在途态重入会泄漏旧 fd 并析构带注册的旧 Channel——
        // 先关闭在途 socket 并摘事件（Channel 留待 connecting 重建）再重启
        m_channel->DisableAllEvent();
        closeInFlightSocket("restart while connecting");
    }
    m_state = kDisconnected;
    m_retry_delay_ms = kInitRetryDelayMs;
    startInLoop();   // 内部检查 m_connect/状态（Stop 后的 Restart 为空操作）
}

void Connector::Stop()
{
    m_connect = false;   // 同步置位：迟到定时器与在途路径据此让路
    // C1 同型纪律（审查 #1）：stopInLoop 会析构 TimerManager——若在退避
    // 定时器的分派帧内（失败回调链上用户调 stop()）内联执行，返回路径
    // 将访问已释放对象（Timer.cc 的 ResetTimer/回调尾部写）。queue-only：
    // 一律排到分派轮外执行（与 TcpServer::Stop 对齐）
    m_loop.Peer().QueueInLoop([self = shared_from_this()]() { self->stopInLoop(); });
}

void Connector::startInLoop()
{
    assert(m_loop.Peer().isInLoopThread());
    if (!m_connect.load() || m_state != kDisconnected)
    {
        return;
    }

    const socket_t sockfd = ::socket(m_server_addr.isIPV6() ? AF_INET6 : AF_INET,
                                     SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0)
    {
        LOG_ERROR(g_log) << "Connector socket() failed, err=" << strerror(errno);
        if (m_failed_cb)
        {
            m_failed_cb(-1);   // 无 fd 可移交（上层须容忍 <0）
        }
        return;
    }
    m_fd = sockfd;

    // 三档分流：立即成功/EINPROGRESS → 经写事件统一判定；临时错误 → 退避重试；
    // 其余 → 致命失败（muduo Connector 分类表）
    const int ret = ::connect(sockfd, m_server_addr.getSockAddr(), m_server_addr.Length());
    const int err = (ret == 0) ? 0 : errno;
    if (err == 0 || err == EINPROGRESS || err == EINTR || err == EISCONN)
    {
        connecting(sockfd);   // 立即成功也走此路径：写事件立即就绪，SO_ERROR=0
    }
    else if (IsTemporaryConnectError(err))
    {
        failOrRetry("temporary connect error");
    }
    else
    {
        LOG_ERROR(g_log) << "Connector connect fatal, err=" << strerror(err)
                         << ", addr=" << m_server_addr.toString();
        failNoRetry("fatal connect error");   // 致命错误不重试（muduo 语义）
    }
}

void Connector::connecting(socket_t sockfd)
{
    assert(m_loop.Peer().isInLoopThread());
    m_state = kConnecting;
    // 重建 Channel（前次失败/重试的旧 Channel 已摘除注册，销毁安全）。
    // 回调捕裸 this：Channel 生命周期严格内含于 Connector 且
    // 在销毁前已 DisableAllEvent（见 ~Connector 断言）——捕 shared_from_this
    // 会造成 Connector→Channel→回调→Connector 引用环（ASAN 泄漏实证）
    m_channel = std::make_unique<Channel>(m_loop, sockfd);
    m_channel->setWriteCallback([this]() { handleWrite(); });
    m_channel->setErrorCallback([this]() { handleError(); });
    m_channel->EnableWriteEvent();
}

void Connector::handleWrite()
{
    assert(m_loop.Peer().isInLoopThread());
    if (m_state != kConnecting)
    {
        return;   // Stop 后的迟到事件
    }
    // D39 纪律：先摘事件再判定；Channel 不在分派轮内销毁（留待下次
    // connecting 重建或析构），且移交后永不再触碰 fd
    m_channel->DisableAllEvent();

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0)
    {
        so_error = errno;
    }
    if (so_error == 0)
    {
        m_state = kConnected;
        const socket_t fd = m_fd;
        m_fd = -1;   // 所有权移交上层
        if (m_new_conn_cb)
        {
            m_new_conn_cb(fd);
        }
        return;
    }
    LOG_WARN(g_log) << "Connector connect failed, err=" << strerror(so_error)
                    << ", addr=" << m_server_addr.toString();
    failOrRetry("connect failed");   // SO_ERROR 可为临时类错误（ECONNREFUSED 等）
}

void Connector::handleError()
{
    assert(m_loop.Peer().isInLoopThread());
    if (m_state != kConnecting)
    {
        return;
    }
    m_channel->DisableAllEvent();
    LOG_WARN(g_log) << "Connector error event, addr=" << m_server_addr.toString();
    failOrRetry("error event");
}

void Connector::retryInLoop()
{
    assert(m_loop.Peer().isInLoopThread());
    if (!m_connect.load())
    {
        return;   // Stop 后的迟到路径
    }
    if (m_timer_manager == nullptr)
    {
        // 惰性创建（loop 线程，D6/D18 纪律）：未启用 retry 的客户端零开销
        m_timer_manager = std::make_unique<TimerManager>(m_loop.Peer());
    }
    LOG_INFO(g_log) << "Connector retry in " << m_retry_delay_ms
                    << "ms, addr=" << m_server_addr.toString();
    const auto delay = std::chrono::milliseconds(m_retry_delay_ms);
    m_retry_delay_ms = std::min(m_retry_delay_ms * 2, kMaxRetryDelayMs);
    // 定时器闭包捕 weak_ptr：lock 失败即 no-op（结构性消除悬垂）
    m_retry_timer = m_timer_manager->addTimer(
        [weak_self = std::weak_ptr<Connector>(shared_from_this())]() {
            if (const auto self = weak_self.lock())
            {
                self->startInLoop();   // 内部检查 m_connect/状态
            }
        },
        TimerManager::ClockType::now() + delay, std::chrono::milliseconds(0));
}

void Connector::stopInLoop()
{
    assert(m_loop.Peer().isInLoopThread());
    if (m_retry_timer)
    {
        m_retry_timer->cancel();   // 从 TimerManager 摘除，阻止待触发重试
        m_retry_timer.reset();
    }
    m_timer_manager.reset();   // 定时器宿主析构（loop 线程，D6/D18 纪律）
    if (m_state == kConnecting)
    {
        m_channel->DisableAllEvent();   // 幂等（可能已摘）
        closeInFlightSocket("stopped");
    }
    m_state = kDisconnected;
    // Channel 不在此销毁（Stop 可能处于分派轮内）：留待析构（分派轮外）
}

void Connector::closeInFlightSocket(const char* reason) noexcept
{
    assert(m_loop.Peer().isInLoopThread());
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_state = kDisconnected;
    LOG_TRACE(g_log) << "Connector close in-flight socket: " << reason;
}

socket_t Connector::handoverInFlightSocket() noexcept
{
    assert(m_loop.Peer().isInLoopThread());
    const socket_t fd = m_fd;
    m_fd = -1;   // 所有权移交（不关闭；接收方 RAII 负责）
    m_state = kDisconnected;
    return fd;
}

void Connector::failOrRetry(const char* reason)
{
    assert(m_loop.Peer().isInLoopThread());
    if (!m_retry)
    {
        // !retry：fd 移交上层构造短命连接（统一失败通知，U3 裁决）
        if (m_failed_cb)
        {
            const socket_t fd = handoverInFlightSocket();
            m_failed_cb(fd);
            return;
        }
        closeInFlightSocket(reason);
        return;
    }
    closeInFlightSocket(reason);
    if (m_connect.load())
    {
        retryInLoop();
    }
}

void Connector::failNoRetry(const char* reason)
{
    assert(m_loop.Peer().isInLoopThread());
    if (m_failed_cb)
    {
        const socket_t fd = handoverInFlightSocket();
        m_failed_cb(fd);
        return;
    }
    closeInFlightSocket(reason);
}

// ---------------------------------------------------------------------------
// TcpClient
// ---------------------------------------------------------------------------

TcpClient::TcpClient(EventLoop* loop, std::string_view server_addr, std::string name)
    : m_loop(loop),
      m_server_addr(Address::FromIpPortString(server_addr)),
      m_name(std::move(name))
{
    assert(m_loop != nullptr);
    m_connector = std::make_shared<Connector>(std::ref(*m_loop), m_server_addr);
    m_connector->setNewConnectionCallback(
        [this](socket_t sockfd) { newConnection(sockfd); });
    m_connector->setFailedCallback(
        [this](socket_t fd) { failedConnection(fd); });
}

TcpClient::~TcpClient()
{
    // 契约：必须已 stop() 且连接已断（否则 close_cb 捕获的 this 悬垂）。
    // release 同样强制（审查 #5）：违约立即中止，而非静默退化为 UAF
    if (!m_stopped.load() || m_connection.load() != nullptr)
    {
        LOG_FATAL(g_log) << "TcpClient [" << m_name << "] contract violated:"
                         << " destructed without stop()/Disconnected (stopped="
                         << m_stopped.load() << ", connection"
                         << (m_connection.load() != nullptr ? " alive" : " null") << ")";
        SystemUtil::Abort();
    }
}

void TcpClient::connect()
{
    if (m_stopped.load())
    {
        LOG_WARN(g_log) << "TcpClient [" << m_name << "] connect ignored: stopped";
        return;   // 单向生命周期（对称 TcpServer 的 Stop→Start）
    }
    m_connector->Start();
}

void TcpClient::disconnect()
{
    if (const auto conn = m_connection.load())
    {
        conn->shutdown();   // 优雅半关：已排队数据发完后对端收 EOF
    }
}

void TcpClient::stop()
{
    m_stopped = true;
    m_connector->Stop();
}

void TcpClient::newConnection(socket_t sockfd)
{
    assert(m_loop->isInLoopThread());
    if (m_stopped.load())
    {
        // 审查 #3：stop 与 removeConnection→Restart 的跨线程窗口兜底——
        // 竞态期发起的迟到连接确定性拒绝，单向生命周期不被穿透
        LOG_WARN(g_log) << "TcpClient [" << m_name << "] reject late connection: stopped";
        ::close(sockfd);
        return;
    }

    // 本端地址（accepted/connected fd 上的 getsockname，family 判定同 TcpServer）
    sockaddr_in6 local_sockaddr{};
    socklen_t len = sizeof(local_sockaddr);
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&local_sockaddr), &len) != 0)
    {
        LOG_ERROR(g_log) << "TcpClient getsockname failed, err=" << strerror(errno);
        failedConnection(sockfd);   // 审查 #6：经统一失败通知上报，消除无声死胡同
        return;
    }
    Address local_addr(local_sockaddr.sin6_family == AF_INET6
                           ? Address(std::move(local_sockaddr))
                           : Address(std::move(*reinterpret_cast<sockaddr_in*>(&local_sockaddr))));

    const uint32_t id = m_next_conn_id.fetch_add(1);
    auto conn = std::make_shared<TCPConnection>(std::ref(*m_loop),
                                                m_name + "#" + std::to_string(id),
                                                sockfd, local_addr, m_server_addr);
    conn->setTcpNoDelay(true);
    conn->setConnectionCallback(m_conn_cb);
    conn->setMessageCallback(m_msg_cb);
    conn->setWriteCompleteCallback(m_write_cb);
    conn->setHighWaterMarkCallback(m_high_water_cb, m_high_water_mark);
    conn->setCloseCallback([this](const TCPConnection::ptr& c) { removeConnection(c); });
    m_connection.store(conn);
    conn->connectEstablished();
}

void TcpClient::failedConnection(socket_t fd)
{
    assert(m_loop->isInLoopThread());
    if (fd < 0)
    {
        // socket() 创建失败（fd 耗尽级）：无 fd 可承载短命连接，仅记录
        LOG_ERROR(g_log) << "TcpClient [" << m_name << "] connect failed: no socket";
        return;
    }
    const uint32_t id = m_next_conn_id.fetch_add(1);

    // 本端地址（连接尝试已隐式 bind，getsockname 可用；失败兜底用目标地址）
    sockaddr_in6 local_sockaddr{};
    socklen_t len = sizeof(local_sockaddr);
    const bool has_local =
        (::getsockname(fd, reinterpret_cast<sockaddr*>(&local_sockaddr), &len) == 0);
    Address local_addr = has_local
                             ? Address(local_sockaddr.sin6_family == AF_INET6
                                           ? Address(std::move(local_sockaddr))
                                           : Address(std::move(*reinterpret_cast<sockaddr_in*>(&local_sockaddr))))
                             : m_server_addr;

    // 短命连接：承载 Disconnected 终态的统一失败通知（用户裁决 #3）；
    // send 被状态机安全拒绝；析构关闭移交的 fd
    auto conn = std::make_shared<TCPConnection>(
        std::ref(*m_loop), m_name + "#" + std::to_string(id) + "-failed",
        fd, local_addr, m_server_addr);
    conn->setConnectionCallback(m_conn_cb);
    conn->connectDestroyed();
}

void TcpClient::removeConnection(const TCPConnection::ptr& conn)
{
    assert(m_loop->isInLoopThread());
    (void)conn;
    m_connection.store(nullptr);
    if (!m_stopped.load() && m_connector->retryEnabled())
    {
        m_connector->Restart();   // 断线自动重连（退避间隔重置，立即重试）
    }
}

} // namespace bus
