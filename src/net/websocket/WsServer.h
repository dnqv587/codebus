//***********************************************
// name      : WsServer.h
// brief     : WebSocket 服务端：UpgradeHandler 桥接（HTTP 层钩子接管连接）+ 注册表
//             + base loop 中心 sweep（空闲 ping/pong 死线/关闭握手死线——镜像 HttpServer）
// attention : 生命周期归 WS 层（U7 契约"接管后归回调方"）：接管即重绑 msg_cb、
//             addCloseObserver、setContext；须挂在 HttpServer::setUpgradeHandler
//***********************************************
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Timer.h"
#include "noncopyable.hpp"
#include "net/TCPConnection.h"
#include "net/http/HttpMessage.h"
#include "net/websocket/WsSession.h"

namespace bus::http {
class HttpRequest;
}

namespace bus::ws {

using bus::EventLoop;  // bus 根类型

/// @brief 服务端配置（聚合体）
struct WsServerConfig {
    WsSessionConfig session;        ///< 会话级配置透传
    uint32_t idlePingSec = 30;      ///< 空闲心跳间隔（无消息即 ping）
    uint32_t pongTimeoutSec = 10;   ///< ping 后应答死线（超→close 1001）
    uint32_t closeReplySec = 5;     ///< 主动 close 后 echo 死线（超→硬关）
};

/// @brief WebSocket 服务端（一个实例可服务一个 HttpServer 的全部升级连接）
class WsServer : noncopyable, public std::enable_shared_from_this<WsServer> {
 public:
    using ptr = std::shared_ptr<WsServer>;

    explicit WsServer(EventLoop* loop, WsServerConfig cfg = {});
    ~WsServer();  // 契约：须已 Stop 且定时器已清

    /// @brief 启动（幂等；惰性建 sweep 定时器——HttpServer::Start 后调用）
    void Start();
    /// @brief 停止（清定时器；会话随连接生命周期自行终结——WS 无集中停机需求）
    void Stop();

    /// @brief 消息回调（ioLoop 线程；业务在此注册）
    void onMessage(WsSession::MessageCallback cb) {
        m_on_message = std::move(cb);
    }
    void onClose(WsSession::CloseCallback cb) {
        m_on_close = std::move(cb);
    }

    /// @brief 向指定连接发文本（ioLoop 语义同上）
    void sendText(const TCPConnection::ptr& conn, std::string_view text);
    /// @brief 向指定连接发二进制
    void sendBinary(const TCPConnection::ptr& conn, std::string_view data);

    /// @brief UpgradeHandler 桥接入口（签名即 http::HttpServer::UpgradeHandler——直接挂）
    void handleUpgrade(const TCPConnection::ptr& conn, http::HttpRequest req, std::string_view leftover);

    /// @brief 当前会话数（观测）
    ATTR_PURE_INLINE
    size_t sessionCount() const noexcept {
        std::lock_guard<std::mutex> lk(m_sessions_mutex);
        return m_sessions.size();
    }

 private:
    void onWsData(const TCPConnection::ptr& conn, Buffer* buf);
    void sweep();
    /// @brief 按连接名取会话（context 直挂——U1）
    std::shared_ptr<WsSession> findSession(const TCPConnection::ptr& conn);

    /// @brief 配置
    const WsServerConfig m_cfg;
    /// @brief base loop（sweep 宿主）
    EventLoop* m_loop;
    /// @brief 注册表（key: 连接名；ioLoop 增删 + base loop sweep 快照——mutex）
    mutable std::mutex m_sessions_mutex;
    struct SessionEntry {
        std::shared_ptr<WsSession> session;
        /// @brief 最近消息/ping 时刻（ns）
        std::atomic<int64_t> lastActiveNs{0};
        /// @brief 最近 ping 发出时刻（0=无在途 ping）
        std::atomic<int64_t> pingSentNs{0};
        /// @brief close 发出时刻（0=未发）
        std::atomic<int64_t> closeSentNs{0};
    };
    std::unordered_map<std::string, std::shared_ptr<SessionEntry>> m_sessions;
    /// @brief 消息回调（业务注册）
    WsSession::MessageCallback m_on_message;
    /// @brief 关闭回调
    WsSession::CloseCallback m_on_close;
    /// @brief sweep 定时器宿主
    std::unique_ptr<TimerManager> m_timers;
    Timer::ptr m_sweep_timer;
    /// @brief 启动标志
    std::atomic<bool> m_started{false};
};

}  // namespace bus::ws
