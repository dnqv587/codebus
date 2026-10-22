//***********************************************
// name      : TcpServer.cc
// brief     : TCP 服务器实现（连接生命周期：base loop 建连/管表，IO loop 分派）
// attention : None
//***********************************************
#include "net/TcpServer.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstring>

#include "exception/Exception.hpp"
#include "log/Logger.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/Listener.h"
#include "net/TCPConnection.h"

namespace bus
{

static Logger& g_log = GET_LOGGER("sys");

namespace detail
{

/// @brief 解析端口串（纯数字且 1..65535，否则抛 AddressError）
/// @param text 端口字符串
/// @return 端口号
/// @throw Exception::AddressError 格式非法或越界
uint16_t ParsePort(std::string_view text)
{
    if (text.empty() || text.size() > 5)
    {
        throw Exception::AddressError(("port parse error: " + std::string(text)).c_str());
    }
    uint32_t port = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            throw Exception::AddressError(("port parse error: " + std::string(text)).c_str());
        }
        port = port * 10 + static_cast<uint32_t>(c - '0');
    }
    if (port == 0 || port > 65535)
    {
        throw Exception::AddressError(("port out of range: " + std::string(text)).c_str());
    }
    return static_cast<uint16_t>(port);
}

} // namespace detail

Address TcpServer::ParseListenAddress(std::string_view addr)
{
    if (!addr.empty() && addr.front() == '[')
    {
        // IPv6："[x::y]:port"
        const auto close = addr.find(']');
        if (close == std::string_view::npos || close + 2 > addr.size() ||
            addr[close + 1] != ':')
        {
            throw Exception::AddressError(("listen address parse error: " + std::string(addr)).c_str());
        }
        const auto ip = addr.substr(1, close - 1);
        const auto port = detail::ParsePort(addr.substr(close + 2));
        return Address(ip, port, true);
    }
    // IPv4："ip:port"（IPv6 字面量必须用方括号形式，裸多冒号判非法）
    const auto colon = addr.find(':');
    if (colon == std::string_view::npos ||
        addr.find(':', colon + 1) != std::string_view::npos)
    {
        throw Exception::AddressError(("listen address parse error: " + std::string(addr)).c_str());
    }
    const auto ip = addr.substr(0, colon);
    const auto port = detail::ParsePort(addr.substr(colon + 1));
    return Address(ip, port);
}

TcpServer::TcpServer(EventLoop* loop, std::string_view addr, uint32_t thread_num)
    : m_loop(loop),
      m_listen_addr(ParseListenAddress(addr)),
      m_name(addr),
      m_thread_num(thread_num)
{
    assert(m_loop != nullptr);
}

TcpServer::~TcpServer()
{
    // 契约：析构前必须 Stop 并等待 DoneCallback（连接表空、监听已摘）
    assert(m_listener == nullptr);
    assert(m_conn_map.empty());
    LOG_INFO(g_log) << "TcpServer [" << m_name << "] destructed";
}

void TcpServer::Start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true))
    {
        return;   // 幂等：重复 Start 无效果
    }
    m_loop->RunInLoop([this]() { startInLoop(); });
}

void TcpServer::startInLoop()
{
    assert(m_loop->isInLoopThread());
    if (m_thread_num > 0)
    {
        m_pool = std::make_unique<EventLoopThreadPool>(m_loop, m_name);
        m_pool->setThreadNum(m_thread_num);
        m_pool->Start(m_thread_init_cb);
    }
    m_listener = std::make_unique<Listener>(std::ref(*m_loop), m_listen_addr);
    m_listener->setNewConnectionCallback(
        [this](socket_t sockfd, Address peer_addr) {
            handleNewConnection(sockfd, std::move(peer_addr));
        });
    m_listener->Listen();
    LOG_INFO(g_log) << "TcpServer [" << m_name << "] started, io_threads=" << m_thread_num;
}

void TcpServer::handleNewConnection(socket_t sockfd, Address peer_addr)
{
    assert(m_loop->isInLoopThread());   // Listener 回调（base loop 线程）
    EventLoop* io_loop = (m_pool != nullptr ? m_pool->getNextLoop() : m_loop);

    // 本端地址：accepted fd 上的 getsockname（内核按 family 布局 sockaddr_in/6）
    sockaddr_in6 local_sockaddr{};
    socklen_t len = sizeof(local_sockaddr);
    if (::getsockname(sockfd, reinterpret_cast<sockaddr*>(&local_sockaddr), &len) != 0)
    {
        LOG_ERROR(g_log) << "TcpServer getsockname failed, err=" << strerror(errno);
        ::close(sockfd);
        return;
    }
    Address local_addr(local_sockaddr.sin6_family == AF_INET6
                           ? Address(std::move(local_sockaddr))
                           : Address(std::move(*reinterpret_cast<sockaddr_in*>(&local_sockaddr))));

    const uint32_t id = m_next_conn_id.fetch_add(1);
    const std::string conn_name =
        m_name + "#" + std::to_string(id) + "-" + peer_addr.toString();

    // 建连与入表都在 base loop（连接表单线程访问）；IO 分派交给 ioLoop
    auto conn = std::make_shared<TCPConnection>(std::ref(*io_loop), conn_name, sockfd,
                                                local_addr, peer_addr);
    conn->setTcpNoDelay(true);
    conn->setConnectionCallback(m_conn_cb);
    conn->setMessageCallback(m_msg_cb);
    conn->setWriteCompleteCallback(m_write_cb);
    conn->setHighWaterMarkCallback(m_high_water_cb, m_high_water_mark);
    conn->setCloseCallback([this](const TCPConnection::ptr& c) { removeConnection(c); });
    m_conn_map.emplace(conn_name, conn);
    io_loop->RunInLoop([conn]() { conn->connectEstablished(); });
    LOG_INFO(g_log) << "TcpServer [" << m_name << "] new connection [" << conn_name
                    << "], total=" << m_conn_map.size();
}

void TcpServer::removeConnection(const TCPConnection::ptr& conn)
{
    // conn 的 ioLoop 线程：连接表归 base loop 管，转交过去
    // （单 loop 模式即本线程，立即执行——析构安全由 D27 守卫 + D38 终态前置保证）
    m_loop->RunInLoop([this, conn]() { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TCPConnection::ptr& conn)
{
    assert(m_loop->isInLoopThread());
    m_conn_map.erase(std::string(conn->name()));
    LOG_INFO(g_log) << "TcpServer [" << m_name << "] remove connection [" << conn->name()
                    << "], remain=" << m_conn_map.size();
    if (m_stopping && m_conn_map.empty())
    {
        finishStop();
    }
}

void TcpServer::Stop(DoneCallback cb)
{
    m_loop->RunInLoop([this, cb = std::move(cb)]() mutable { stopInLoop(std::move(cb)); });
}

void TcpServer::stopInLoop(DoneCallback cb)
{
    assert(m_loop->isInLoopThread());
    if (m_stopping)
    {
        LOG_WARN(g_log) << "TcpServer [" << m_name << "] already stopping, ignore";
        return;   // 幂等：首个 Stop 的回调生效
    }
    m_stopping = true;
    m_done_cb = std::move(cb);

    m_listener.reset();   // 摘监听：停止接受新连接
    // 快照遍历：单 loop 模式下 close 链会同步修改连接表
    std::vector<TCPConnection::ptr> conns;
    conns.reserve(m_conn_map.size());
    for (auto& [name, conn] : m_conn_map)
    {
        (void)name;
        conns.push_back(conn);
    }
    for (auto& conn : conns)
    {
        conn->close();   // 走 handleClose 链，removeConnection 汇聚回 base loop
    }
    if (m_conn_map.empty())
    {
        finishStop();   // 无连接：立即收尾
    }
}

void TcpServer::finishStop()
{
    assert(m_loop->isInLoopThread());
    m_pool.reset();   // 停各 IO loop 并 join（IO 线程不依赖 base loop，无死锁）
    LOG_INFO(g_log) << "TcpServer [" << m_name << "] stopped";
    if (m_done_cb && !m_done_invoked)
    {
        m_done_invoked = true;
        const DoneCallback cb = m_done_cb;
        cb();
    }
}

} // namespace bus
