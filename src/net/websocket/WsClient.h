//***********************************************
// name      : WsClient.h
// brief     : WebSocket 客户端（H5.2：TcpClient 底座 + 握手校验 + WsSession 客户端
//             角色复用；明文 ws，wss 留 TLS 客户端轮）
// attention : 回调在客户端 ioLoop 触发不得阻塞；回调内只许 stop()（TcpClient 契约）；
//             析构前须 stop 且连接已断开；v1 无自动心跳（调用方按需 ping）；
//             断线不自动重连（单次生命周期，重连须重建实例）
//***********************************************
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "noncopyable.hpp"
#include "net/TcpClient.h"
#include "net/http/HttpResponseParser.h"
#include "net/websocket/WsSession.h"

namespace bus::ws {

/// @brief 客户端配置（聚合体）
struct WsClientConfig {
    /// @attention session.client 由构造器强制 true（客户端视角）——调用方设值无效，
    ///            其余字段（maxMessageBytes/validateText）生效
    WsSessionConfig session;
};

/// @brief WebSocket 客户端（入口镜像 WsSession 发送面 + TcpClient 生命周期）
class WsClient : noncopyable {
 public:
    using OpenCallback = std::function<void()>;
    using MessageCallback = WsSession::MessageCallback;
    using CloseCallback = std::function<void(uint16_t code, std::string_view reason)>;
    using ErrorCallback = std::function<void(std::string_view reason)>;

    /// @param loop 客户端事件循环
    /// @param addr 服务器地址 "ip:port"
    WsClient(EventLoop* loop, std::string_view addr, WsClientConfig cfg = {});
    ~WsClient() noexcept;

    /// @brief 握手成功回调（connect 前装配）
    void setOpenCallback(OpenCallback cb) {
        m_on_open = std::move(cb);
    }
    /// @brief 消息回调（文本/二进制，已重组）
    void setMessageCallback(MessageCallback cb) {
        m_on_message = std::move(cb);
    }
    /// @brief 关闭回调（close 握手收口或异常断连 1006）
    void setCloseCallback(CloseCallback cb) {
        m_on_close = std::move(cb);
    }
    /// @brief 握手失败回调（非 101/Accept 失配/握手期断连）
    void setErrorCallback(ErrorCallback cb) {
        m_on_error = std::move(cb);
    }
    /// @brief pong 到达回调（心跳观测）
    void setPongCallback(std::function<void()> cb) {
        m_on_pong = std::move(cb);
    }

    /// @brief 发起连接 + 升级握手（异步；101 校验通过触发 onOpen）
    void connect(std::string_view path = "/");
    /// @brief 停止并断开（loop 线程清理）
    void stop();

    /// @brief 发送文本/二进制消息（客户端掩码帧；未就绪 WARN 忽略）
    void sendText(std::string_view text);
    void sendBinary(std::string_view data);
    /// @brief 心跳 ping（pong 经 setPongCallback 观测）
    void ping();
    /// @brief 主动关闭握手（发 close 帧；对端 echo 后经 onClose 收口并关 TCP）
    void close(uint16_t code = 1000, std::string_view reason = "");

    /// @brief 当前底层连接（测试观测用；跨线程安全）
    ATTR_NODISCARD
    TCPConnection::ptr connection() const noexcept {
        return m_client ? m_client->connection() : nullptr;
    }

 private:
    void onConnection(const TCPConnection::ptr& conn);
    void onMessage(const TCPConnection::ptr& conn, Buffer* buf);
    /// @brief 握手失败收口（error cb 恰一次 + 关连接）
    void failHandshake(const TCPConnection::ptr& conn, std::string_view reason);
    /// @brief 帧发送函数注入（会话无连接引用）
    std::function<void(std::string_view)> sendFn();

    /// @brief 客户端事件循环
    EventLoop* m_loop;
    /// @brief Host 头值
    const std::string m_host;
    /// @brief 配置
    const WsClientConfig m_cfg;
    /// @brief TCP 基座（无 retry：单次生命周期）
    std::unique_ptr<TcpClient> m_client;
    /// @brief 握手响应解析器
    bus::http::HttpResponseParser m_parser;
    /// @brief 帧会话（客户端角色）
    WsSession m_session;
    /// @brief 握手 key（Accept 校验用）
    std::string m_key;
    /// @brief 升级请求路径
    std::string m_path;
    /// @brief 握手完成（loop 线程）
    bool m_open = false;
    /// @brief 握手失败已收口
    bool m_handshake_failed = false;
    /// @brief close 回调已交付（防断连重复）
    bool m_close_delivered = false;
    /// @brief 握手成功回调
    OpenCallback m_on_open;
    /// @brief 消息回调
    MessageCallback m_on_message;
    /// @brief 关闭回调
    CloseCallback m_on_close;
    /// @brief 握手失败回调
    ErrorCallback m_on_error;
    /// @brief pong 到达回调
    std::function<void()> m_on_pong;
};

}  // namespace bus::ws