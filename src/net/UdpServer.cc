//***********************************************
// name      : UdpServer.cc
// brief     : UDP 服务端实现（同步模型：接收主线程 + 任务线程池）
// attention : None
//***********************************************
#include "net/UdpServer.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "Exception.hpp"
#include "log/Logger.h"
#include "thread/ThreadPool.h"
#include "Buffer.hpp"

namespace bus
{

static Logger& g_log = GET_LOGGER("sys");

namespace
{

/// @brief 接收主线程的 poll 超时[ms]——停止标志的检查粒度
constexpr int kPollTimeoutMs = 200;

/// @brief 单个 UDP 数据报缓冲上限（64KB，无截断）
constexpr size_t kMaxDatagram = 64 * 1024;

} // namespace

UdpServer::UdpServer(std::string_view addr, uint32_t worker_num)
    : m_local_addr(Address::FromIpPortString(addr)),   // M8：单次解析复用
      m_worker_num(worker_num == 0 ? 1 : worker_num)
{
    m_socket = std::make_unique<Socket>(
        m_local_addr.isIPV6() ? Socket::IPV6 : Socket::IPV4, Socket::NIO_UDP);
    if (!m_socket->isAvailable())
    {
        // M9：socket 创建失败（fd=-1）——Socket 构造不校验（层缺陷登记），
        // 此处兜底上抛兑现 @throw SocketError 契约（避免 bind 断言/误导错因）
        throw Exception::SocketError(("UdpServer socket create failed: " + std::string(addr)).c_str());
    }
    m_socket->setReuseAddr(true);
    // m_socket->setReusePort(true);
    m_socket->setKeepAlive(true);
    if (!m_socket->bind(m_local_addr))
    {
        // 构造期失败上抛（I1 纪律：bind 失败不能静默——幽灵启动无从感知）
        throw Exception::SocketError(("UdpServer bind failed: " + std::string(addr)).c_str());
    }

    // 实际绑定地址（含内核分配端口的场景）
    sockaddr_in6 local_sockaddr{};
    socklen_t len = sizeof(local_sockaddr);
    if (::getsockname(m_socket->fd(), reinterpret_cast<sockaddr*>(&local_sockaddr),
                      &len) == 0)
    {
        m_local_addr = (local_sockaddr.sin6_family == AF_INET6)
                           ? Address(std::move(local_sockaddr))
                           : Address(std::move(*reinterpret_cast<sockaddr_in*>(&local_sockaddr)));
    }

    // 创建不启动（W1.1 用户裁决）：线程对象与池构造期就绪，Start() 启动
    m_pool = std::make_unique<ThreadPool>("UdpServer-" + m_local_addr.toString());
    m_recv_thread = std::make_unique<Thread>(
        [this]() { recvLoop(); }, "UdpServer-recv");
}

UdpServer::~UdpServer()
{
    if (m_started.load() && !m_stopped.load())
    {
        Stop();   // 自动收尾（未 Start 则跳过——对象已建、线程未跑）
    }
}

void UdpServer::Start()
{
    if (m_stopped.load())
    {
        LOG_WARN(g_log) << "UdpServer Start ignored: stopped";   // 单向生命周期
        return;
    }
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true))
    {
        return;   // 幂等
    }
    try
    {
        m_pool->start(m_worker_num);
        m_recv_thread->run();
    }
    catch (...)
    {
        // 启动失败（pthread_create 耗尽等）：回收已启动部分并重建未启动
        // 对象——回滚到「已创建未启动」基态，Start 可重试（I4 纪律）
        if (m_pool != nullptr)
        {
            m_pool->stopAndDrain();   // 已启动的工作者排空退出；未启动为空操作
        }
        m_pool = std::make_unique<ThreadPool>("UdpServer-" + m_local_addr.toString());
        m_recv_thread = std::make_unique<Thread>(
            [this]() { recvLoop(); }, "UdpServer-recv");
        m_started = false;
        throw;
    }
    LOG_INFO(g_log) << "UdpServer started on " << m_local_addr.toString()
                    << ", workers=" << m_worker_num;
}

void UdpServer::setMessageCallback(MessageCallback cb) noexcept
{
    if (m_started.load())
    {
        LOG_WARN(g_log) << "UdpServer setMessageCallback ignored: already started "
                        << "(worker threads read concurrently)";
        return;
    }
    m_msg_cb = std::move(cb);
}

void UdpServer::Stop()
{
    // 未启动守卫（审查 #3）：未 Start 时接收线程从未 run——join 对 m_tid==0
    // 断言/抛出；仅置停（封死后续 Start，单向生命周期一致）
    if (!m_started.load())
    {
        m_stopped = true;
        return;
    }
    bool expected = false;
    if (!m_stopped.compare_exchange_strong(expected, true))
    {
        return;   // 幂等
    }
    // 停收 → join 接收线程（poll 超时粒度内退出，不再提交新任务）→
    // 停池（BlockingQueue::over 后队列取尽才退——排空语义，D35 修复保证）
    if (m_recv_thread != nullptr)
    {
        m_recv_thread->join();
    }
    if (m_pool != nullptr)
    {
        m_pool->stopAndDrain();   // D41 闭环：已入队任务由工作者线程排空执行
    }
    LOG_INFO(g_log) << "UdpServer stopped on " << m_local_addr.toString();
}

void UdpServer::sendTo(std::string_view data, const Address& peer)
{
    if (m_stopped.load())
    {
        LOG_WARN(g_log) << "UdpServer sendTo ignored: stopped";
        return;
    }
    // 数据报内核原子 + MSG_DONTWAIT——任意线程安全（含工作者线程内回包）
    if (!m_socket->sendTo(data.data(), data.size(), peer))
    {
        // M11：失败不静默（I1 精神）——EMSGSIZE/EAGAIN/ENETUNREACH 等可观测
        LOG_WARN(g_log) << "UdpServer sendTo failed, peer=" << peer.toString()
                        << ", err=" << strerror(errno);
    }
}

void UdpServer::recvLoop()
{
    std::array<char, kMaxDatagram> buf{};
    while (!m_stopped.load())
    {
        pollfd pfd{};
        pfd.fd = m_socket->fd();
        pfd.events = POLLIN;
        const int ret = ::poll(&pfd, 1, kPollTimeoutMs);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_ERROR(g_log) << "UdpServer poll error, err=" << strerror(errno);
            continue;
        }
        if (ret == 0)
        {
            continue;   // 超时：回到停止检查
        }
        if ((pfd.revents & POLLERR) != 0)
        {
            LOG_ERROR(g_log) << "UdpServer poll error event, err="
                             << m_socket->getSocketErrno();
            // M7：消费一次错误状态（recvFrom 读出挂起的 ICMP 错误）——
            // 持续错误态若不消费，poll 将立即再就绪（忙转 + 日志风暴）
            char err_drain[1];
            (void)m_socket->recvFrom(err_drain, sizeof(err_drain));
        }
        if ((pfd.revents & POLLIN) == 0)
        {
            continue;
        }

        // 就绪后排空（Listener 纪律）：每条数据报拷贝入任务、交工作者处理
        while (!m_stopped.load())
        {
            const auto [n, peer] = m_socket->recvFrom(buf.data(), buf.size());
            if (n < 0)
            {
                const int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK)
                {
                    break;   // 本轮排空
                }
                if (err == EINTR)
                {
                    continue;
                }
                LOG_ERROR(g_log) << "UdpServer recvFrom error, err=" << strerror(err);
                break;
            }
            // 按报文实长构造 Buffer，shared_ptr 持有并随任务闭包共享（用户裁决
            // W1.3：所有权在调用点显式化）——回调指针的生命周期由闭包内的
            // shared_ptr 保证；需保留可 std::move(*data) 接管
            auto datagram = std::make_shared<Buffer>(static_cast<size_t>(n));
            datagram->Write(Slice(buf.data(), static_cast<size_t>(n)));
            m_pool->run([this, data = std::move(datagram), peer]() {
                if (m_msg_cb)
                {
                    m_msg_cb(data, peer);
                }
            });
        }
    }
}

} // namespace bus
