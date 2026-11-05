//***********************************************
// name      : HttpClient.cc
// brief     : 异步 HTTP 客户端实现（组合 TcpClient）
// attention : None
//***********************************************
#include "net/http/HttpClient.h"

#include "log/Logger.h"
#include "net/EventLoop.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");

HttpClient::HttpClient(EventLoop* loop, std::string_view addr, HttpClientConfig cfg)
    : m_cfg(cfg),
      m_loop(loop),
      m_host(addr),
      m_client(std::make_unique<TcpClient>(loop, addr, "HttpClient")) {
    m_client->enableRetry();  // 断线自动重连（Connector 退避）
    m_client->setConnectionCallback([this](const TCPConnection::ptr& c) { onConnection(c); });
    m_client->setMessageCallback([this](const TCPConnection::ptr& c, Buffer* b) { onMessage(c, b); });
}

HttpClient::~HttpClient() {
    // 契约：须已 Stop 且连接断开（TcpClient 析构断言兜底）
}

void HttpClient::Start() {
    m_stopped.store(false);
    m_client->connect();
}

void HttpClient::Stop() {
    if (!m_stopped.exchange(true)) {
        // 清理须在 loop 线程（TimerManager 析构纪律——D40 同型）；且必须
        // QueueInLoop：RunInLoop 在 loop 线程内是立即执行——用户在响应/超时回调
        // 里调 Stop() 会在 TimerManager 自身的定时器帧内析构它（UAF，H2 审查 P1）
        m_loop->QueueInLoop([this] {
            failInflight("client stopped");
            cancelResponseTimer();
            m_timers.reset();
            m_client->stop();
            m_client->disconnect();
        });
    }
}

void HttpClient::Get(std::string_view path_and_query, ResponseCallback cb) {
    HttpRequest req;
    req.setMethod(HttpMethod::kGET);
    req.appendUrl(path_and_query);
    req.setPath(std::string(path_and_query));
    Request(std::move(req), std::move(cb));
}

void HttpClient::Post(std::string_view path, std::string_view content_type, std::string_view body,
                      ResponseCallback cb) {
    HttpRequest req;
    req.setMethod(HttpMethod::kPOST);
    req.appendUrl(path);
    req.setPath(std::string(path));
    req.addHeaderRaw("Content-Type", content_type);
    req.appendBody(body);
    Request(std::move(req), std::move(cb));
}

void HttpClient::Request(HttpRequest req, ResponseCallback cb) {
    if (m_stopped.load()) {
        LOG_WARN(g_logger) << "HttpClient: Request after Stop ignored";
        return;
    }
    bool expected = false;
    if (!m_inflight.compare_exchange_strong(expected, true)) {
        LOG_WARN(g_logger) << "HttpClient: request already in flight, rejected";
        return;  // 单在途
    }
    TCPConnection::ptr conn = m_client->connection();
    if (conn && conn->isConnected()) {
        // 连接就绪：直接经连接的 loop 投递（send 与状态写串行化）
        conn->getLoop()->RunInLoop(
            [this, req = std::move(req), cb = std::move(cb)]() mutable {
                m_pending_req = std::move(req);
                m_pending_cb = std::move(cb);
                armResponseTimer();
                TCPConnection::ptr c = m_client->connection();
                if (c && c->isConnected()) {
                    m_parser.start(m_pending_req.getMethod());
                    c->send(m_pending_req.Dump(m_host));
                } else {
                    failInflight("connection lost before send");
                }
            });
    } else {
        // 未就绪：mutex 交接暂存（conn_cb 的 flush 在 loop 线程取走）
        std::lock_guard<std::mutex> lk(m_handoff_mutex);
        m_handoff_req = std::move(req);
        m_handoff_cb = std::move(cb);
        m_handoff_ready = true;
    }
}

void HttpClient::Reset() {
    m_client->disconnect();
}

void HttpClient::onConnection(const TCPConnection::ptr& conn) {
    if (!conn->isConnected()) {
        if (m_disconnected_observer) {
            m_disconnected_observer();
        }
        failInflight("connection lost");  // 在途失败（retry 会自动重连）
        cancelResponseTimer();
        return;
    }
    // 连接就绪：交接暂存请求（若有）
    std::optional<std::pair<HttpRequest, ResponseCallback>> handoff;
    {
        std::lock_guard<std::mutex> lk(m_handoff_mutex);
        if (m_handoff_ready) {
            handoff.emplace(std::move(m_handoff_req), std::move(m_handoff_cb));
            m_handoff_ready = false;
        }
    }
    if (handoff) {
        m_pending_req = std::move(handoff->first);
        m_pending_cb = std::move(handoff->second);
        armResponseTimer();
        m_parser.start(m_pending_req.getMethod());
        conn->send(m_pending_req.Dump(m_host));
    }
}

void HttpClient::onMessage(const TCPConnection::ptr& conn, Buffer* buf) {
    while (buf->ReadableSize() > 0) {
        auto readable = buf->PeekAll();
        ParseResult r = m_parser.Execute(reinterpret_cast<const char*>(readable.data()), readable.size());
        buf->SkipRead(r.consumed);
        if (r.event == ParseEvent::kComplete) {
            HttpResponse resp = m_parser.takeResponse();
            cancelResponseTimer();
            ResponseCallback cb = std::move(m_pending_cb);
            m_pending_cb = nullptr;
            m_inflight.store(false, std::memory_order_release);
            if (cb) {
                cb(std::move(resp));
            }
            if (!conn->isConnected()) {
                return;
            }
            continue;  // keep-alive 续喂
        }
        if (r.event == ParseEvent::kError) {
            LOG_WARN(g_logger) << "HttpClient: response parse error, closing";
            failInflight("response parse error");
            conn->shutdown();
            return;
        }
        if (r.event == ParseEvent::kUpgrade) {
            LOG_WARN(g_logger) << "HttpClient: upgrade response unsupported, closing";
            failInflight("upgrade response");
            conn->shutdown();
            return;
        }
        return;  // kNeedMore
    }
}

void HttpClient::failInflight(const char* why) {
    ResponseCallback cb;
    ResponseCallback handoff_cb;
    {
        std::lock_guard<std::mutex> lk(m_handoff_mutex);
        if (m_handoff_ready) {
            handoff_cb = std::move(m_handoff_cb);  // 未 flush 的请求也须回调（exactly-once）
            m_handoff_ready = false;
        }
    }
    const bool had_inflight = m_inflight.exchange(false, std::memory_order_acq_rel);
    cb = std::move(m_pending_cb);  // 早退路径：CAS 失败也可能已写入 pending（竞态窗口 B）
    m_pending_cb = nullptr;
    if (had_inflight) {
        LOG_WARN(g_logger) << "HttpClient: in-flight request failed: " << why;
        cancelResponseTimer();
    }
    // 逐个回调（防重入：先清空再调用；同一逻辑请求只会经 cb 或 handoff_cb 之一）
    if (handoff_cb) {
        handoff_cb(std::nullopt);
    }
    if (cb) {
        cb(std::nullopt);
    }
}

void HttpClient::armResponseTimer() {
    if (m_cfg.responseTimeoutMs == 0) {
        return;  // 0 = 不限时
    }
    cancelResponseTimer();
    // TimerManager 惰性：须在 loop 线程创建（调用点 conn_cb/RunInLoop 均已保证）
    TCPConnection::ptr c = m_client->connection();
    if (!c) {
        return;
    }
    if (!m_timers) {
        m_timers = std::make_unique<TimerManager>(m_loop);
    }
    m_response_timer = m_timers->addTimer(
        [this] { failInflight("response timeout"); m_client->disconnect(); },
        TimerManager::ClockType::now() + std::chrono::milliseconds(m_cfg.responseTimeoutMs),
        std::chrono::milliseconds(0));
}

void HttpClient::cancelResponseTimer() {
    if (m_response_timer) {
        m_response_timer->cancel();
        m_response_timer = nullptr;
    }
}

}  // namespace bus::http
