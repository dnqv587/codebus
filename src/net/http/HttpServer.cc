//***********************************************
// name      : HttpServer.cc
// brief     : HTTP/1.1 服务器实现：会话管理、消息循环分发、错误路径、
//             keep-alive/慢速攻击中心扫描、Stop 生命周期对齐
// attention : None
//***********************************************
#include "net/http/HttpServer.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

#include "log/Logger.h"
#include "net/EventLoop.h"
#include "net/http/HttpRequestParser.h"
#include "thread/ThreadPool.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");

namespace {

/// @brief steady 时钟纳秒
inline int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(TimerManager::ClockType::now().time_since_epoch())
        .count();
}

/// @brief 秒转纳秒
inline int64_t secToNs(uint32_t s) noexcept {
    return static_cast<int64_t>(s) * 1000'000'000;
}

/// @brief 解析错误 → HTTP 状态码
HttpStatus statusFor(HttpParseError e) noexcept {
    switch (e) {
        case HttpParseError::kBodyTooLarge:
            return HttpStatus::kPAYLOAD_TOO_LARGE;  // 413
        case HttpParseError::kUrlTooLong:
            return HttpStatus::kURI_TOO_LONG;  // 414
        case HttpParseError::kHeaderTooLarge:
            return HttpStatus::kREQUEST_HEADER_FIELDS_TOO_LARGE;  // 431
        default:
            return HttpStatus::kBAD_REQUEST;  // 400
    }
}

}  // namespace

/// @brief 便捷注册的 path 级方法路由器：同路径 Get/Post/... 各自分派，
///        未注册方法回 405 + Allow（FunctionServlet 本身不校验方法）
class MethodRouterServlet : public Servlet {
 public:
    void add(HttpMethod m, HttpServer::Handler h) {
        m_routes[m] = std::move(h);
    }
    void Handle(const HttpRequest& req, HttpResponse& resp) override {
        auto it = m_routes.find(req.getMethod());
        if (it == m_routes.end() && req.getMethod() == HttpMethod::kHEAD) {
            it = m_routes.find(HttpMethod::kGET);  // HEAD 回落 GET（Java doHead 默认语义；Dump 剥 body）
        }
        if (it == m_routes.end()) {
            std::string allow;
            for (const auto& [m, h] : m_routes) {
                if (!allow.empty()) {
                    allow += ", ";
                }
                allow += std::string(HttpMethodToString(m));
            }
            resp.setHeader("Allow", allow);
            resp.SendError(HttpStatus::kMETHOD_NOT_ALLOWED);
            return;
        }
        it->second(req, resp);
    }

 private:
    /// @brief 方法 → handler（有序 map：Allow 列表输出稳定）
    std::map<HttpMethod, HttpServer::Handler> m_routes;
};

/// @brief 每连接 HTTP 会话（仅本文件可见）
struct HttpServer::HttpSession {
    HttpSession(size_t maxBody, size_t maxUrl) : parser(maxBody, maxUrl) {}

    /// @brief 请求解析器（cfg 限额注入）
    HttpRequestParser parser;
    /// @brief 所属连接（弱引用，防生命周期悬挂）
    std::weak_ptr<TCPConnection> conn;
    /// @brief 任意收包时刻（steady ns；空闲超时基准）
    std::atomic<int64_t> lastActiveNs{0};
    /// @brief 请求开始时刻（0=空闲；慢速攻击防护基准）
    std::atomic<int64_t> requestStartNs{0};
    /// @brief 半关时刻（0=未半关；扫描兜底 close 基准）
    std::atomic<int64_t> shutdownNs{0};
    /// @brief worker 单飞标志（同连接至多一个在途响应；V6）
    std::atomic<bool> responsePending{false};
    /// @brief worker 单飞期间暂存字节（不能留 TCPConnection 输入 Buffer 等 EPOLLIN：用户态数据无新事件，连接假死；V6）
    std::string pendingInput;
};

HttpServer::HttpServer(EventLoop* loop, std::string_view addr, uint32_t thread_num, HttpServerConfig cfg)
    : m_cfg(std::move(cfg)),
      m_loop(loop),
      m_dispatcher(std::make_shared<ServletDispatcher>()),
      m_server(std::make_unique<TcpServer>(loop, addr, thread_num)) {
    m_server->setConnectionCallback([this](const TCPConnection::ptr& c) { onConnection(c); });
    m_server->setMessageCallback([this](const TCPConnection::ptr& c, Buffer* b) { onMessage(c, b); });
}

HttpServer::~HttpServer() {
    // 契约：须已 Stop 且 DoneCallback 已触发（TcpServer 析构断言兜底）
}

void HttpServer::Start() {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) {
        return;  // 幂等
    }
    if (m_cfg.workerNum > 0)  // 业务线程池（Tomcat 心智：handler 可阻塞不占 IO 线程）
    {
        m_pool = std::make_unique<ThreadPool>("http-worker");
        m_pool->start(m_cfg.workerNum);
    }
    m_server->Start();
    m_loop->RunInLoop([this]  // loop 线程构造纪律（TcpClient 先例）
                      {
                          m_timers = std::make_unique<TimerManager>(*m_loop);
                          uint32_t half = std::max(1u, std::min(m_cfg.keepAliveIdleSec, m_cfg.headerTimeoutSec) / 2);
                          m_sweep_timer = m_timers->addTimer(
                              [this] { sweep(); }, TimerManager::ClockType::now() + std::chrono::seconds(half),
                              std::chrono::milliseconds(half * 1000));
                      });
}

void HttpServer::Stop(DoneCallback cb) {
    if (!m_stopped.exchange(true))  // stopAndDrain 不可重入，重复 Stop 须自挡
    {
        // 顺序关键（P0 修复）：先排空业务池再停 TCP——TcpServer::finishStop 会先
        // 析构 IO 线程池（各 EventLoop 栈对象销毁）再触发 DoneCallback，若 drain
        // 放在其后，残余 worker 任务的 send/RunInLoop 将对已析构 loop 投递（UAF）。
        // 此处 drain 时全部 IO loop 存活；drain 后至 TcpServer::Stop 前窗口内 IO 线程
        // 的新投递由 ThreadPool::run 的非运行态守卫丢弃（WARN），连接随停机关闭。
        if (m_pool) {
            m_pool->stopAndDrain();  // 阻塞调用线程直至在途任务全部执行
        }
        m_server->Stop([this, cb = std::move(cb)]  // DoneCallback 在 base loop（D39 透传）
                       {
                           if (m_timers) {
                               if (m_sweep_timer) {
                                   m_sweep_timer->cancel();
                               }
                               m_timers.reset();  // base loop 线程析构（D40 同型纪律）
                           }
                           if (cb) {
                               cb();
                           }
                       });
    }
}

void HttpServer::registerMethod(HttpMethod m, std::string_view path, Handler h) {
    std::string p(path);
    auto it = m_method_routes.find(p);
    if (it == m_method_routes.end()) {
        auto router = std::make_shared<MethodRouterServlet>();
        it = m_method_routes.emplace(p, router).first;
        m_dispatcher->addExactServlet(path, router);
    }
    it->second->add(m, std::move(h));
}

void HttpServer::Get(std::string_view path, Handler h) {
    registerMethod(HttpMethod::kGET, path, std::move(h));
}

void HttpServer::Post(std::string_view path, Handler h) {
    registerMethod(HttpMethod::kPOST, path, std::move(h));
}

void HttpServer::Put(std::string_view path, Handler h) {
    registerMethod(HttpMethod::kPUT, path, std::move(h));
}

void HttpServer::Delete(std::string_view path, Handler h) {
    registerMethod(HttpMethod::kDELETE, path, std::move(h));
}

std::shared_ptr<HttpServer::HttpSession> HttpServer::findSession(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_sessions_mutex);
    auto it = m_sessions.find(name);
    return it == m_sessions.end() ? nullptr : it->second;
}

void HttpServer::onConnection(const TCPConnection::ptr& conn) {
    if (m_conn_observer) {
        m_conn_observer(conn, conn->isConnected());
    }
    if (!conn->isConnected())  // 断开：出表（纯数据对象，mutex 下析构安全）
    {
        std::lock_guard<std::mutex> lk(m_sessions_mutex);
        m_sessions.erase(std::string(conn->name()));
        return;
    }
    auto session = std::make_shared<HttpSession>(m_cfg.maxBodyBytes, m_cfg.maxUrlBytes);
    session->conn = conn;
    session->lastActiveNs = nowNs();
    std::weak_ptr<TCPConnection> weak = conn;
    // 捕裸指针而非 shared_ptr：parser 属于 session（成员），回调触发时 session 必存活；
    // 捕 shared_ptr 会造成 session 自环泄漏（ASAN 已抓获）
    HttpSession* raw = session.get();
    session->parser.setOnMessageBegin([raw] { raw->requestStartNs = nowNs(); });
    // 头完成即收口慢速头观察窗口：408 只覆盖头阶段，慢 body 不限时（maxBody 封顶）
    session->parser.setOnHeadersComplete([raw] { raw->requestStartNs = 0; });
    session->parser.setOnExpectContinue(  // 立即回 100（curl 大 POST hang 防护）
        [weak] {
            if (auto c = weak.lock()) {
                c->send(std::string_view("HTTP/1.1 100 Continue\r\n\r\n"));
            }
        });
    std::lock_guard<std::mutex> lk(m_sessions_mutex);
    m_sessions.emplace(std::string(conn->name()), session);
}

void HttpServer::onMessage(const TCPConnection::ptr& conn, Buffer* buf) {
    auto session = findSession(std::string(conn->name()));
    if (!session) {
        return;  // 会话已被摘除（扫描器判死等），忽略本包
    }
    session->lastActiveNs = nowNs();

    if (session->responsePending.load())  // 单飞中：字节暂存（防假死）+ 上限检查
    {
        auto all = buf->PeekAll();
        stashPending(conn, session, reinterpret_cast<const char*>(all.data()), all.size());
        buf->SkipRead(buf->ReadableSize());
        return;
    }

    const char* base = reinterpret_cast<const char*>(buf->PeekAll().data());
    size_t total = buf->ReadableSize();
    size_t consumed = 0;
    if (!processInput(conn, session, base, total, &consumed) && consumed < total) {
        // 进入单飞且本包尚有剩余：转入 pendingInput 等回灌
        stashPending(conn, session, base + consumed, total - consumed);
    }
    buf->SkipRead(buf->ReadableSize());  // 本包全消费（未完成部分在 parser 累加器）
}

void HttpServer::stashPending(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session,
                              const char* data, size_t len) {
    session->pendingInput.append(data, len);
    if (session->pendingInput.size() > m_cfg.maxBodyBytes)  // 单飞窗口内存放大防护
    {
        LOG_FMT_WARN(g_logger, "HttpServer: pendingInput overflow %zu on %s, closing", session->pendingInput.size(),
                     std::string(conn->name()).c_str());
        session->pendingInput.clear();
        sendSimple(conn, nullptr, HttpStatus::kPAYLOAD_TOO_LARGE, "pending input overflow");
        shutdownSoon(conn, session);
    }
}

bool HttpServer::processInput(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session,
                              const char* data, size_t len, size_t* consumedOut) {
    size_t off = 0;
    while (off < len) {
        ParseResult r = session->parser.Execute(data + off, len - off);
        off += r.consumed;
        switch (r.event) {
            case ParseEvent::kComplete: {
                dispatch(conn, session, session->parser.TakeRequest());
                if (session->responsePending.load()) {
                    *consumedOut = off;  // 剩余字节归调用方暂存（单飞）
                    return false;
                }
                continue;  // IO 模式天然保序，继续吃 pipeline
            }
            case ParseEvent::kError: {
                sendSimple(conn, nullptr, statusFor(r.error), {});
                shutdownSoon(conn, session);  // framing 不可信，半关闭
                *consumedOut = len;
                return true;
            }
            case ParseEvent::kUpgrade: {
                sendSimple(conn, nullptr, HttpStatus::kNOT_IMPLEMENTED, "protocol upgrade not supported");
                shutdownSoon(conn, session);
                *consumedOut = len;
                return true;
            }
            case ParseEvent::kNeedMore:
                *consumedOut = len;  // parser 累加器已持有未完成部分
                return true;
        }
    }
    *consumedOut = off;
    return true;
}

void HttpServer::dispatch(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session,
                          HttpRequest req) {
    if (m_pool)  // worker 模式：投递业务池（handler 可阻塞不占 IO 线程）
    {
        session->responsePending = true;  // 单飞：同连接至多一个在途响应（保序）
        EventLoop* io_loop = conn->getLoop();
        std::weak_ptr<TCPConnection> weak_conn = conn;
        std::weak_ptr<HttpSession> weak_session = session;
        m_pool->run([this, weak_conn, weak_session, req = std::move(req), io_loop]() mutable {
            HttpResponse resp(m_cfg.serverName);
            m_dispatcher->Handle(req, resp);
            bool close_after = !req.isKeepAlive() || !resp.isKeepAlive();
            if (auto c = weak_conn.lock()) {
                c->send(resp.Dump(req));  // send 跨线程安全（拷贝入队 RunInLoop）
                if (close_after) {
                    if (auto s = weak_session.lock()) {
                        s->shutdownNs = nowNs();
                    }
                    c->shutdown();
                }
            }
            io_loop->RunInLoop(
                [this, weak_conn, weak_session]  // 回 IO 线程
                {
                    auto c = weak_conn.lock();
                    if (!c) {
                        return;  // 连接已析构（session 可能仍被 sweep 快照保活）
                    }
                    auto s = weak_session.lock();
                    if (!s) {
                        return;  // 会话已随连接销毁
                    }
                    s->responsePending = false;
                    if (!s->pendingInput.empty())  // 回灌：单飞期间到达的字节
                    {
                        std::string data = std::move(s->pendingInput);
                        s->pendingInput.clear();
                        size_t consumed = 0;
                        if (!processInput(c, s, data.data(), data.size(), &consumed) && consumed < data.size()) {
                            stashPending(c, s, data.data() + consumed, data.size() - consumed);
                        }
                    }
                });
        });
        return;
    }

    // IO 内联路径（低延迟默认）
    HttpResponse resp(m_cfg.serverName);
    m_dispatcher->Handle(req, resp);
    conn->send(resp.Dump(req));
    if (!req.isKeepAlive() || !resp.isKeepAlive()) {
        shutdownSoon(conn, session);
    }
}

void HttpServer::sendSimple(const TCPConnection::ptr& conn, const HttpRequest* req, HttpStatus status,
                            std::string_view msg) {
    HttpResponse resp(m_cfg.serverName);
    resp.SendError(status, msg);
    if (req) {
        conn->send(resp.Dump(*req));
    } else {
        HttpRequest dummy;  // 错误路径一律 close 语义
        dummy.setKeepAlive(false);
        conn->send(resp.Dump(dummy));
    }
}

void HttpServer::shutdownSoon(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session) {
    if (session) {
        session->shutdownNs = nowNs();
    }
    conn->shutdown();  // 排队数据发完再关写方向（D40 安全路径；跨线程安全）
}

void HttpServer::sweep() {
    int64_t now = nowNs();
    std::vector<std::shared_ptr<HttpSession>> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_sessions_mutex);
        snapshot.reserve(m_sessions.size());
        for (auto& kv : m_sessions) {
            snapshot.push_back(kv.second);
        }
    }
    for (auto& s : snapshot) {
        auto conn = s->conn.lock();
        if (!conn || !conn->isConnected()) {
            continue;
        }
        int64_t rs = s->requestStartNs.load();
        if (rs != 0 && now - rs > secToNs(m_cfg.headerTimeoutSec)) {
            // slowloris：请求开始后头迟迟不完整 → 408 + 半关闭（窗口仅覆盖头阶段）
            sendSimple(conn, nullptr, HttpStatus::kREQUEST_TIMEOUT, "header timeout");
            shutdownSoon(conn, s);
        } else if (rs == 0 && s->shutdownNs.load() == 0 && !s->responsePending.load() &&
                   now - s->lastActiveNs.load() > secToNs(m_cfg.keepAliveIdleSec)) {
            // 空闲超时：优雅半关闭（单飞在途豁免——长 handler 是 worker 模式合法场景）；
            // 走 shutdownSoon 记录时刻，激活 5s 滞留兜底 close（防半开连接泄漏）
            shutdownSoon(conn, s);
        } else if (s->shutdownNs.load() != 0 && now - s->shutdownNs.load() > secToNs(5)) {
            conn->close();  // 半关滞留兜底硬收
        }
    }
}

}  // namespace bus::http
