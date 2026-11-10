//***********************************************
// name      : HttpClient.h
// brief     : 异步 HTTP 客户端（组合 TcpClient：Connector 建连 + retry 断线重连
//             + EventLoop 驱动 + 回调式响应）
// attention : 回调在 IO 线程触发——回调内不得阻塞、不得同步等待本 client；
//             同一时刻至多一个在途请求（重入被拒+WARN）；HTTP 4xx/5xx 是
//             成功响应非错误；析构前须 Stop 且连接已断开（TcpClient 契约）
//***********************************************
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "Macro.h"
#include "noncopyable.hpp"
#include "Timer.h"
#include "net/TcpClient.h"
#include "net/http/HttpRequestParser.h"  // ParseResult 复用
#include "net/http/HttpResponseParser.h"

namespace bus {

namespace ssl {
class SslContext;
}  // namespace ssl

namespace http {

/// @brief TLS 配置（https；默认关）
struct ClientTlsConfig {
    bool enabled = false;      ///< 总开关
    bool verifyPeer = true;    ///< 对端证书校验（false 仅测试/受控内网——WARN 留痕）
    std::string caFile;        ///< 信任 CA 文件（空 = 系统默认 CA 路径）
};

/// @brief 客户端配置（聚合体）
struct HttpClientConfig {
    uint32_t responseTimeoutMs = 10000;  ///< 响应超时（在途超时即断连宁丢不错位）
    size_t maxBodyBytes = 8_MB;          ///< 响应体上限
    ClientTlsConfig tls;                 ///< https（默认关；开启即整条连接走 TLS）
};

/// @brief 异步 HTTP 客户端（单连接串行复用；keep-alive）
class HttpClient : noncopyable {
 public:
    /// @brief 响应回调（传输/超时/解析失败 → nullopt；HTTP 4xx/5xx → 有效响应）
    using ResponseCallback = std::function<void(std::optional<HttpResponse>)>;
    /// @brief 连接断开观测（测试用；IO 线程触发）
    using DisconnectedObserver = std::function<void()>;

    /// @param loop 客户端事件循环（连接与解析均在其线程）
    /// @param addr 服务器地址 "ip:port"
    HttpClient(EventLoop* loop, std::string_view addr, HttpClientConfig cfg = {});
    ~HttpClient();

    /// @brief 发起连接（异步；Connector retry 兜底；幂等）
    void Start();

    /// @brief 停止并断开（在途回调 nullopt）
    void Stop();

    /// @brief GET 快捷形态
    void Get(std::string_view path_and_query, ResponseCallback cb);
    /// @brief POST 快捷形态
    void Post(std::string_view path, std::string_view content_type, std::string_view body,
              ResponseCallback cb);
    /// @brief 通用形态：连接就绪即发送；未就绪暂存（建连后自动 flush）
    /// @attention 单在途：在途中再 Request 被拒（WARN）
    void Request(HttpRequest req, ResponseCallback cb);

    /// @brief 弃用复用连接（下次 Request 重建）
    void Reset();

    /// @brief 当前底层连接（未建立/已断开为空；跨线程安全——TcpClient 原子读）
    /// @attention 仅观测用：不得直接对连接收发（绕过单在途状态机）
    ATTR_NODISCARD
    TCPConnection::ptr connection() const noexcept {
        return m_client ? m_client->connection() : nullptr;
    }

    /// @brief 连接断开观测（测试用）
    void setDisconnectedObserver(DisconnectedObserver ob) noexcept {
        m_disconnected_observer = std::move(ob);
    }

 private:
    void onConnection(const TCPConnection::ptr& conn);
    void onMessage(const TCPConnection::ptr& conn, Buffer* buf);
    /// @brief 尝试冲刷暂存请求（loop 线程）：连接就绪则取走 handoff 发送；
    ///        未就绪留在暂存区等 onConnection（D54：Request 与重连完成的
    ///        竞态窗口由「park 后必投递一次 flush」闭合）
    void TryFlushHandoff();
    /// @brief 在途失败收口（cb(nullopt)+清在途+断连）
    /// @param why 失败原因（日志）
    /// @param fail_parked 连带判死暂存区（false=断连但重连在望：暂存请求从未发出，
    ///                    按契约留待重连后 flush——D54；Stop/TLS fatal 须 true 收口）
    void failInflight(const char* why, bool fail_parked = true);
    /// @brief 挂响应超时定时器（惰性 TimerManager；TcpClient 先例）
    void armResponseTimer();
    void cancelResponseTimer();

    /// @brief 配置
    const HttpClientConfig m_cfg;
    /// @brief 客户端事件循环（清理投递目标）
    EventLoop* m_loop;
    /// @brief Host 头值（"ip:port"）
    const std::string m_host;
    /// @brief TCP 基座
    std::unique_ptr<TcpClient> m_client;
    /// @brief 响应解析器（单连接串行复用）
    HttpResponseParser m_parser;
    /// @brief 超时定时器宿主（loop 线程惰性建/Stop 内析构）
    std::unique_ptr<TimerManager> m_timers;
    Timer::ptr m_response_timer;
    /// @brief 在途标志（跨线程 CAS 互斥；状态本身归 loop 线程）
    std::atomic<bool> m_inflight{false};
    /// @brief 暂存请求（连接未就绪时；loop 线程）
    HttpRequest m_pending_req;
    /// @brief 暂存回调（同上）
    ResponseCallback m_pending_cb;
    /// @brief 断开观测
    DisconnectedObserver m_disconnected_observer;
    /// @brief 连接未就绪时的请求交接（调用线程 → loop 线程；mutex 保护交接窗口）
    std::mutex m_handoff_mutex;
    HttpRequest m_handoff_req;
    ResponseCallback m_handoff_cb;
    bool m_handoff_ready = false;
    /// @brief 停止标志
    std::atomic<bool> m_stopped{false};
    /// @brief TLS fatal 标志（证书校验失败/协议错误——R6：停重试+新请求即拒；
    ///        不复用 m_stopped：其被 Stop() 的 exchange 自挡消费，此处置位会导致
    ///        后续 Stop() 跳过 loop 线程清理（m_timers 泄漏在用户线程析构——D40））
    std::atomic<bool> m_tls_dead{false};
    /// @brief TLS 客户端上下文（tls.enabled 时构造期创建；空 = 明文模式）
    std::shared_ptr<ssl::SslContext> m_tls_ctx;
};

}  // namespace http
}  // namespace bus
