//***********************************************
// name      : HttpConnectionPool.h
// brief     : 单 host HTTP 连接池（ADR-0003：管理 N 个 HttpClient 实例的调度层，
//             不改单实例状态机；空闲优先 + FIFO 排队 + 惰性创建）
// attention : 池与全部实例共用一个 EventLoop（池状态 loop 线程独占）；
//             回调在 loop 线程触发不得阻塞；传输失败 nullopt 原样交付（池层不重试，
//             Connector retry 管连接层重连）；析构前须 Stop 完成（done cb 触发）
//***********************************************
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Macro.h"
#include "noncopyable.hpp"
#include "net/http/HttpClient.h"

namespace bus::http {

/// @brief 池配置（聚合体）
struct HttpPoolConfig {
    uint32_t maxConnections = 4;  ///< 连接上限（惰性创建至此）
    HttpClientConfig client;      ///< 每实例配置（超时/TLS/重定向）
};

/// @brief 单 host 连接池（入口镜像 HttpClient）
class HttpConnectionPool : noncopyable {
 public:
    using ResponseCallback = HttpClient::ResponseCallback;
    using DoneCallback = std::function<void()>;

    /// @param loop 池与全部实例的事件循环
    /// @param addr 服务器地址 "ip:port"（单 host）
    HttpConnectionPool(EventLoop* loop, std::string_view addr, HttpPoolConfig cfg = {});
    ~HttpConnectionPool() noexcept;

    /// @brief 启用（连接惰性创建：首请求分发时建实例）
    void Start();
    /// @brief 停止：排队请求 nullopt 交付，在途经实例 Stop 收口（exactly-once）；
    ///        全部连接断开后触发 done cb（HttpServer::Stop 同款形态）
    void Stop(DoneCallback cb = {});

    /// @brief GET 快捷形态
    void Get(std::string_view path_and_query, ResponseCallback cb);
    /// @brief POST 快捷形态
    void Post(std::string_view path, std::string_view content_type, std::string_view body,
              ResponseCallback cb);
    /// @brief 通用形态：空闲实例优先，全忙 FIFO 排队（响应完成即分发）
    void Request(HttpRequest req, ResponseCallback cb);

 private:
    struct Entry {
        std::unique_ptr<HttpClient> client;
        bool busy = false;  ///< loop 线程独占
    };
    /// @brief 入队 + 分发（跨线程入口经 QueueInLoop 串行化）
    void enqueue(HttpRequest req, ResponseCallback cb);
    /// @brief loop 线程：取排队请求分发到空闲实例（无空闲且未达上限则惰性创建）
    void dispatch();
    /// @brief loop 线程：屏障已过 + 待断开归零 + cb 在 → 触发 done（恰一次）
    void maybeFireStopDone();

    /// @brief 池事件循环（= 实例 loop）
    EventLoop* m_loop;
    /// @brief 服务器地址
    const std::string m_addr;
    /// @brief 配置
    const HttpPoolConfig m_cfg;
    /// @brief 连接实例（loop 线程）
    std::vector<std::unique_ptr<Entry>> m_entries;
    /// @brief FIFO 排队（loop 线程）
    std::deque<std::pair<HttpRequest, ResponseCallback>> m_queue;
    /// @brief 停止标志
    std::atomic<bool> m_stopped{false};
    /// @brief Stop 收口计数（loop 线程：待断开连接数）
    size_t m_stop_pending = 0;
    /// @brief Stop 清理闭包已执行（loop 线程；重复 Stop cb 链入判定用）
    bool m_stop_closed = false;
    /// @brief 实例 Stop 清理 FIFO 屏障已过（loop 线程；done 先决条件）
    bool m_stop_barrier = false;
    /// @brief Stop 完成回调（loop 线程）
    DoneCallback m_stop_cb;
};

}  // namespace bus::http