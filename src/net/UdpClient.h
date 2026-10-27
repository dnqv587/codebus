//***********************************************
// name      : UdpClient.h
// brief     : UDP 客户端（同步模型）：无线程，调用线程直接收发（可选 bind、
//             可选 connect 连接型）
// attention : 无内部线程与回调——「回调接收」场景由 UdpServer（显式绑定
//             指定端口）覆盖；sendTo/send 数据报内核原子（任意线程安全），
//             connect 与收发建议同线程或外部同步（对端状态非原子）；
//             receive 阻塞调用线程，勿多线程并发 receive
//***********************************************
#pragma once
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "Macro.h"
#include "noncopyable.hpp"
#include "net/Address.h"
#include "net/Socket.h"

namespace bus {

class Buffer;

//***********************************************
// name      : UdpClient
// brief     : UDP 客户端：调用线程同步收发（自由型 sendTo / 连接型 connect+send）
// attention : None
//***********************************************
class UdpClient : noncopyable {
 public:
    /// @brief 收到的数据报（shared_ptr<Buffer> 形态与 UdpServer::MessageCallback 对称）
    using Datagram = std::pair<std::shared_ptr<Buffer>, Address>;

    /// @brief 构造 UDP 客户端（创建非阻塞 socket，可选绑定）
    /// @param bind_addr 绑定地址 "ip:port"（IPv6 "[x::y]:port"）：空=不绑定
    ///        （IPv4，首次 sendTo 时内核分配临时端口）；非空=显式绑定（须
    ///        具体端口——port 0 撞 Address 解析的 I2 语义抛 AddressError）；
    ///        多播接收场景 bind "0.0.0.0:port" 后 joinGroup
    /// @throw Exception::AddressError 地址格式非法
    /// @throw Exception::SocketError socket 创建或 bind 失败
    explicit UdpClient(std::string_view bind_addr = "");

    /// @brief 发送数据报到指定对端（自由型；任意线程安全）
    /// @param data 数据（单次一条数据报）
    /// @param peer 目的地址
    /// @return 全量发送返回 true；EMSGSIZE/EAGAIN/ENETUNREACH 等失败返回
    ///         false 并记警告（连接型发非默认对端的 EISCONN 同此路径）
    ATTR_NODISCARD
    bool sendTo(std::string_view data, const Address& peer) noexcept;

    /// @brief 发送数据报到已连接对端（连接型，走 write）
    /// @attention 未 connect 返回 false 并记警告
    ATTR_NODISCARD
    bool send(std::string_view data) noexcept;

    /// @brief 连接对端（连接型）：此后内核仅投递该对端报文（过滤他源），
    ///        send 走 write；ICMP 端口不可达在 receive 路径暴露
    ///        （ECONNREFUSED 记警告并返回 nullopt，客户端可恢复）
    /// @param peer 对端地址
    /// @return connect 成功返回 true
    /// @attention 重复调用=重关联对端（同一 socket 换目的）；与收发建议
    ///            同线程或外部同步（对端状态非原子）
    bool connect(const Address& peer) noexcept;

    /// @brief 接收一条数据报（阻塞调用线程至有数据或超时）
    /// @param timeout_ms 超时[ms]：-1=无限期；0=非阻塞单次；>0=毫秒超时
    /// @return 收到返回 {数据报, 对端}（含 0 长数据报）；超时或收路径错误
    ///         （ECONNREFUSED 等已记日志）返回 nullopt
    /// @attention poll 伪唤醒按剩余时间继续等待，不会提前返回 nullopt
    ATTR_NODISCARD
    std::optional<Datagram> receive(int timeout_ms) noexcept;

    /// @brief 允许发送广播（fd 级线程安全）
    void setBroadcast(bool on) noexcept { m_socket->setBroadcast(on); }

    /// @brief 加入 IPv4 多播组（多播接收：先 bind "0.0.0.0:port" 再加入）
    bool joinGroup(const Address& multicastAddr) noexcept { return m_socket->joinGroup(multicastAddr); }

    /// @brief 离开 IPv4 多播组
    bool leaveGroup(const Address& multicastAddr) noexcept { return m_socket->leaveGroup(multicastAddr); }

    /// @brief 设置多播 TTL
    void setMulticastTTL(int ttl) noexcept { m_socket->setMulticastTTL(ttl); }

    /// @brief 本地地址（getsockname；未绑定为 0.0.0.0:0，sendTo 后含内核
    ///        分配的临时端口）
    ATTR_NODISCARD
    std::optional<Address> localAddr() const noexcept { return m_socket->getLocalAddr(); }

 private:
    /// @brief UDP socket（fd 由本对象独占，析构关闭）
    std::unique_ptr<Socket> m_socket;
    /// @brief 连接型对端（connect 后有效；非原子——与收发同线程或外部同步）
    Address m_peer;
    /// @brief 是否已连接对端
    bool m_connected{false};
};

}  // namespace bus
