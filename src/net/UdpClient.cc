//***********************************************
// name      : UdpClient.cc
// brief     : UDP 客户端实现（同步收发：poll + recvFrom deadline 循环）
// attention : None
//***********************************************
#include "net/UdpClient.h"

#include <poll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include "Exception.hpp"
#include "log/Logger.h"
#include "Buffer.hpp"

namespace bus
{

static Logger* g_log = GET_LOGGER("sys");

namespace
{

/// @brief 单个 UDP 数据报缓冲上限（64KB——IPv4 数据报理论上限，无截断）
constexpr size_t kMaxDatagram = 64 * 1024;

/// @brief 有限超时下的 poll 分片[ms]——无限期等待用 poll(-1) 不分片
constexpr int kPollSliceMs = 200;

} // namespace

UdpClient::UdpClient(std::string_view bind_addr)
{
    const bool explicit_bind = !bind_addr.empty();
    const Address bind = explicit_bind ? Address::FromIpPortString(bind_addr) : Address();
    m_socket = std::make_unique<Socket>(
        (explicit_bind && bind.isIPV6()) ? Socket::IPV6 : Socket::IPV4, Socket::NIO_UDP);
    if (!m_socket->isAvailable())
    {
        // Socket 构造不校验 fd（层缺陷登记 Q32）——此处兜底上抛兑现 @throw 契约
        throw Exception::SocketError("UdpClient socket create failed");
    }
    m_socket->setReuseAddr(true);
    if (explicit_bind && !m_socket->bind(bind))
    {
        // 构造期失败上抛（I1 纪律：bind 失败不能静默——幽灵客户端无从感知）
        throw Exception::SocketError(("UdpClient bind failed: " + std::string(bind_addr)).c_str());
    }
}

bool UdpClient::sendTo(std::string_view data, const Address& peer) noexcept
{
    if (!m_socket->sendTo(data.data(), data.size(), peer))
    {
        // 失败不静默（I1 精神）——EMSGSIZE/EISCONN（连接型发他址）/EAGAIN 等可观测
        LOG_WARN(g_log) << "UdpClient sendTo failed, peer=" << peer.toString()
                        << ", len=" << data.size() << ", err=" << strerror(errno);
        return false;
    }
    return true;
}

bool UdpClient::send(std::string_view data) noexcept
{
    if (!m_connected)
    {
        LOG_WARN(g_log) << "UdpClient send ignored: not connected";
        return false;
    }
    // 数据报内核原子：write 要么整报发出要么失败，无部分写
    const ssize_t n = m_socket->write(data.data(), data.size());
    if (n != static_cast<ssize_t>(data.size()))
    {
        LOG_WARN(g_log) << "UdpClient send failed, peer=" << m_peer.toString()
                        << ", len=" << data.size() << ", err=" << strerror(errno);
        return false;
    }
    return true;
}

bool UdpClient::connect(const Address& peer) noexcept
{
    if (!m_socket->connect(peer))
    {
        LOG_WARN(g_log) << "UdpClient connect failed, peer=" << peer.toString()
                        << ", err=" << strerror(errno);
        return false;
    }
    const bool reassociate = m_connected;   // 重复调用=重关联对端（同 socket 换目的）
    m_peer = peer;
    m_connected = true;
    LOG_INFO(g_log) << "UdpClient " << (reassociate ? "re-associate" : "connect")
                    << " peer=" << peer.toString() << " (udp)";
    return true;
}

std::optional<UdpClient::Datagram> UdpClient::receive(int timeout_ms) noexcept
{
    // 不预清零（与 UdpServer 线程级一次清零不同：本函数每调用一次）——
    // 仅读取 recvFrom 实际写入的前 n 字节，未写区不触碰
    std::array<char, kMaxDatagram> buf;
    // -1 无限期（poll(-1)）；0 非阻塞单次（跳过 poll）；>0 有限超时（记 deadline）
    const bool infinite = (timeout_ms < 0);
    const bool nonblock_once = (timeout_ms == 0);
    auto deadline = std::chrono::steady_clock::now();
    if (!infinite && !nonblock_once)
    {
        deadline += std::chrono::milliseconds(timeout_ms);
    }

    for (;;)
    {
        if (!nonblock_once)
        {
            int wait = -1;
            if (!infinite)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0)
                {
                    return std::nullopt;   // 超时
                }
                wait = static_cast<int>(std::min<long long>(remaining, kPollSliceMs));
            }
            pollfd pfd{};
            pfd.fd = m_socket->fd();
            pfd.events = POLLIN;
            const int ret = ::poll(&pfd, 1, wait);
            if (ret < 0)
            {
                if (errno == EINTR)
                {
                    continue;   // 无限期/有限期均按剩余时间重等
                }
                LOG_ERROR(g_log) << "UdpClient poll error, err=" << strerror(errno);
                return std::nullopt;
            }
            if (ret == 0)
            {
                continue;   // 分片超时 → 回 deadline 检查（无限期 poll(-1) 不会到这）
            }
            if ((pfd.revents & POLLERR) != 0)
            {
                // 连接型 ICMP 不可达等挂起错误（M7 同款）：消费一次错误状态，
                // 记警告后返回——客户端不损坏，后续收发可恢复
                LOG_WARN(g_log) << "UdpClient receive error event, err="
                                << strerror(m_socket->getSocketErrno());
                char drain[1];
                (void)m_socket->recvFrom(drain, sizeof(drain));
                return std::nullopt;
            }
            if ((pfd.revents & POLLIN) == 0)
            {
                continue;
            }
        }

        const auto [n, peer] = m_socket->recvFrom(buf.data(), buf.size());
        if (n >= 0)
        {
            // 按报文实长构造 Buffer（含 0 长数据报）；shared_ptr 持有（W1.3 形态）
            auto datagram = std::make_shared<Buffer>(static_cast<size_t>(n));
            datagram->Write(Slice(buf.data(), static_cast<size_t>(n)));
            return std::make_pair(std::move(datagram), peer);
        }
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
        {
            if (nonblock_once)
            {
                return std::nullopt;   // 非阻塞单次：无数据即返
            }
            continue;   // 伪唤醒/被信号打断 → 按剩余时间继续
        }
        LOG_WARN(g_log) << "UdpClient receive error, err=" << strerror(err);
        return std::nullopt;
    }
}

} // namespace bus
