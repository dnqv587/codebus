//***********************************************
// name      : HttpServer.h
// brief     : HTTP/1.1 服务器（Tomcat Connector+Container 对应物）：组合 TcpServer、
//             每连接解析会话、Filter+Servlet 分发、keep-alive/慢速攻击防护扫描
// attention : 路由/Filter 注册仅 Start 前（热路径零锁）；析构前须 Stop 且
//             DoneCallback 已触发（同 TcpServer 契约）；关闭一律走 shutdown()
//             半关闭（D40 安全路径），扫描兜底 5s 后 close()
//***********************************************
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "noncopyable.hpp"
#include "type.h"
#include "Timer.h"
#include "net/TcpServer.h"
#include "net/http/Filter.h"
#include "net/http/ServletDispatcher.h"

namespace bus
{

class ThreadPool;

namespace http
{

/// @brief HTTP 服务器配置（聚合体，指定初始化器按需覆盖）
struct HttpServerConfig
{
    std::string serverName = "codebus-http";   ///< Server 响应头
    uint32_t keepAliveIdleSec = 75;            ///< 空闲连接超时（秒）
    uint32_t headerTimeoutSec = 30;            ///< 单请求头部时限（秒；慢速攻击→408）
    size_t maxBodyBytes = 8_MB;                ///< 请求体上限（→413）
    size_t maxUrlBytes = 8_KB;                 ///< URL 上限（→414）
    uint32_t workerNum = 0;                    ///< 业务线程池；0 = IO 线程内联（V6 生效）
};

/// @brief HTTP 服务器（组合 TcpServer；生命周期契约透传）
class HttpServer : noncopyable
{
 public:
    using ptr = std::shared_ptr<HttpServer>;
    using DoneCallback = std::function<void ()>;
    using Handler = FunctionServlet::Handler;

    /// @brief 构造（完成地址解析与端口绑定；装配须在 Start 前）
    /// @param loop 主 loop（acceptor 所在）
    /// @param addr 监听地址 "ip:port"（IPv6 用 "[x::y]:port"）
    /// @param thread_num IO 线程数（0 = 单 loop）
    /// @param cfg 配置
    /// @throw Exception::AddressError 地址格式非法；Exception::SocketError 端口绑定失败
    HttpServer(EventLoop* loop, std::string_view addr, uint32_t thread_num,
               HttpServerConfig cfg = HttpServerConfig());
    ~HttpServer();

    /// @brief 启动（幂等；内部 TcpServer::Start + base loop 惰性建扫描定时器）
    void Start();

    /// @brief 全停：透传 TcpServer::Stop；其 DoneCallback 内清扫描定时器与 worker 池后触发本 cb
    /// @param cb 完成回调（base loop 内触发一次）
    /// @attention 生命周期单向：Stop 后不可重启；重复 Stop 被忽略
    void Stop(DoneCallback cb = DoneCallback());

    // ---- 装配（均须 Start 前）----
    /// @brief 外置分发器（不设则用内置实例）
    void setDispatcher(ServletDispatcher::ptr d) { m_dispatcher = std::move(d); }
    /// @brief 内置分发器便捷访问（直接 addXxxServlet/addFilter）
    ServletDispatcher* dispatcher() noexcept { return m_dispatcher.get(); }
    /// @brief 全局过滤器链尾追加
    void addFilter(Filter::ptr f) { m_dispatcher->addFilter(std::move(f)); }
    /// @brief 便捷路由注册（Spring MVC 心智；FunctionServlet 适配）
    void Get(std::string_view path, Handler h);
    void Post(std::string_view path, Handler h);
    void Put(std::string_view path, Handler h);
    void Delete(std::string_view path, Handler h);
    /// @brief 连接建立/断开观测钩子（观测者不得发送/关闭；测试用）
    void setConnectionObserver(std::function<void (const TCPConnection::ptr&, bool up)> ob)
    {
        m_conn_observer = std::move(ob);
    }

 private:
    struct HttpSession;    // .cc 私有定义（parser + 会话原子状态）

    /// @brief 连接建立/断开（ioLoop 线程）：建/摘会话
    void onConnection(const TCPConnection::ptr& conn);
    /// @brief 数据到达（ioLoop 线程）：循环喂 parser 与分发
    void onMessage(const TCPConnection::ptr& conn, Buffer* buf);
    /// @brief 解析并分发一段字节（Buffer 驱动与 pendingInput 回灌共用）
    /// @param consumedOut 调用方可视为已消费的字节数（kNeedMore 时 parser 已
    ///                    消化进累加器计全量）
    /// @return false = 已进入单飞（consumedOut 之后字节由调用方暂存 pendingInput）
    bool processInput(const TCPConnection::ptr& conn,
                      const std::shared_ptr<HttpSession>& session,
                      const char* data, size_t len, size_t* consumedOut);
    /// @brief 完整请求分发（IO 模式内联执行；worker 模式 V6 投递）
    void dispatch(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session,
                  HttpRequest req);
    /// @brief 框架级错误响应（无关联请求时按 HTTP/1.1 + close 序列化）
    void sendSimple(const TCPConnection::ptr& conn, const HttpRequest* req,
                    HttpStatus status, std::string_view msg);
    /// @brief 半关闭并记 shutdown 时刻（供扫描兜底）
    void shutdownSoon(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session);
    /// @brief base loop 周期扫描：408 慢速头 / 空闲超时 / 半关滞留 5s 兜底
    void sweep();
    /// @brief 按连接名查会话
    std::shared_ptr<HttpSession> findSession(const std::string& name);

    /// @brief 配置
    const HttpServerConfig m_cfg;
    /// @brief 主 loop（acceptor 所在）
    EventLoop* m_loop;
    /// @brief 分发器（内置或外置）
    ServletDispatcher::ptr m_dispatcher;
    /// @brief TCP 基座（组合）
    std::unique_ptr<TcpServer> m_server;
    /// @brief 会话表（key: 连接名；IO 线程增删查 + base loop 扫描快照，mutex 保护）
    std::mutex m_sessions_mutex;
    /// @brief 每连接会话
    std::unordered_map<std::string, std::shared_ptr<HttpSession>> m_sessions;
    /// @brief 扫描定时器管理器（Start 惰性建/Stop DoneCallback 析构；loop 线程纪律）
    std::unique_ptr<TimerManager> m_timers;
    /// @brief 周期扫描定时器
    Timer::ptr m_sweep_timer;
    /// @brief 业务线程池（workerNum>0；V6 生效）
    std::unique_ptr<ThreadPool> m_pool;
    /// @brief 连接观察者
    std::function<void (const TCPConnection::ptr&, bool up)> m_conn_observer;
    /// @brief 启动标志（幂等）
    std::atomic<bool> m_started{false};
};

} // namespace http
} // namespace bus
