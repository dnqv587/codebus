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

namespace bus::http {

/// @brief 客户端配置（聚合体）
struct HttpClientConfig {
    uint32_t responseTimeoutMs = 10000;  ///< 响应超时（在途超时即断连宁丢不错位）
    size_t maxBodyBytes = 8_MB;          ///< 响应体上限
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

    /// @brief 连接断开观测（测试用）
    void setDisconnectedObserver(DisconnectedObserver ob) noexcept {
        m_disconnected_observer = std::move(ob);
    }

 private:
    void onConnection(const TCPConnection::ptr& conn);
    void onMessage(const TCPConnection::ptr& conn, Buffer* buf);
    /// @brief 在途失败收口（cb(nullopt)+清在途+断连）
    void failInflight(const char* why);
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
};

}  // namespace bus::http
