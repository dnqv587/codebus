//***********************************************
// name      : WsSession.h
// brief     : WebSocket 每连接会话：帧流消费（分片重组/控制帧处理）与发送 API
//             （sendText/sendBinary/ping/close）；UTF-8 校验开关；关闭状态机
// attention : 非线程安全——全部在所属连接的 ioLoop 线程内访问（与 HTTP 会话同纪律）
//***********************************************
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "noncopyable.hpp"
#include "type.h"
#include "net/websocket/WsFrame.h"

namespace bus::ws {

/// @brief 会话配置（聚合体）
struct WsSessionConfig {
    size_t maxMessageBytes = 16_MB;  ///< 消息重组上限（防分片炸弹→1009）
    bool validateText = true;        ///< 文本帧 UTF-8 校验（非法→1007）
};

/// @brief 消息回调的简化消息形态
struct WsMessage {
    /// @brief 消息类型
    bool isText = false;
    /// @brief 负载（已解掩码/已重组）
    std::string payload;
};

/// @brief 消费结果（WsSession::Consume 返回——驱动方据此收尾）
enum class ConsumeResult {
    kContinue,   ///< 正常（或已回 pong/已应答 ping）
    kCloseSent,  ///< 已发 close 帧（对端违规或对端 close 的 echo）——连接应等 echo/超时后关
};

/// @brief 每连接会话
class WsSession : noncopyable {
 public:
    using MessageCallback = std::function<void(const WsMessage&)>;
    using CloseCallback = std::function<void(uint16_t code, std::string_view reason)>;

    explicit WsSession(WsSessionConfig cfg = WsSessionConfig());

    /// @brief 消费一段输入字节（驱动方在 msg_cb 中调用；可含任意多帧/半帧）
    /// @param send 发送函数（会话无连接引用——由驱动方注入 conn->send）
    ConsumeResult Consume(const char* data, size_t len, const std::function<void(std::string_view)>& send);

    /// @brief 发送文本/二进制消息（整帧 FIN）
    void sendText(std::string_view text, const std::function<void(std::string_view)>& send);
    void sendBinary(std::string_view data, const std::function<void(std::string_view)>& send);
    /// @brief 心跳 ping
    void ping(const std::function<void(std::string_view)>& send);
    /// @brief 主动关闭（发 close 帧；等对端 echo——超时由 WsServer sweep 硬关）
    void close(uint16_t code, std::string_view reason, const std::function<void(std::string_view)>& send);

    /// @brief 是否已发/已收 close（关闭握手进行中）
    ATTR_NODISCARD
    bool closeSent() const noexcept {
        return m_close_sent;
    }
    void setMessageCallback(MessageCallback cb) {
        m_on_message = std::move(cb);
    }
    void setCloseCallback(CloseCallback cb) {
        m_on_close = std::move(cb);
    }
    /// @brief pong 到达回调（心跳记账——WsServer sweep 据此清 ping 死线；
    ///        任意 pong 均触发，RFC 允许主动 pong）
    void setPongCallback(std::function<void()> cb) {
        m_on_pong = std::move(cb);
    }

 private:
    ConsumeResult handleFrame(const FrameHeader& hdr, const char* payload,
                              const std::function<void(std::string_view)>& send);
    void deliverMessage(const std::function<void(std::string_view)>& send);

    /// @brief 配置
    const WsSessionConfig m_cfg;
    /// @brief 输入缓冲（跨包半帧）
    std::string m_input;
    /// @brief 分片重组累积（首帧 opcode 记 m_frag_text）
    std::string m_frag;
    /// @brief 分片消息是否文本
    bool m_frag_text = false;
    /// @brief 分片进行中
    bool m_fragging = false;
    /// @brief 已发 close
    bool m_close_sent = false;
    /// @brief 消息回调
    MessageCallback m_on_message;
    /// @brief 关闭回调
    CloseCallback m_on_close;
    /// @brief pong 到达回调（心跳记账）
    std::function<void()> m_on_pong;
};

}  // namespace bus::ws
