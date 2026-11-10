//***********************************************
// name      : HttpClient.cc
// brief     : 异步 HTTP 客户端实现（组合 TcpClient）
// attention : None
//***********************************************
#include "net/http/HttpClient.h"

#include "log/Logger.h"
#include "net/EventLoop.h"
#include "net/ssl/SslContext.h"
#include "net/ssl/SslSession.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("sys");

namespace {

/// @brief "ip:port" → "ip"（[v6]:port 剥括号——SNI/主机名校验用裸主机名）
std::string HostPartOf(std::string_view addr) {
    if (!addr.empty() && addr.front() == '[') {  // "[x::y]:port"
        size_t close = addr.find(']');
        return std::string(addr.substr(1, close == std::string_view::npos ? addr.size() - 1 : close - 1));
    }
    size_t colon = addr.rfind(':');
    return colon == std::string_view::npos ? std::string(addr) : std::string(addr.substr(0, colon));
}

}  // namespace

HttpClient::HttpClient(EventLoop* loop, std::string_view addr, HttpClientConfig cfg)
    : m_cfg(cfg),
      m_loop(loop),
      m_host(addr),
      m_client(std::make_unique<TcpClient>(loop, addr, "HttpClient")) {
    m_client->enableRetry();  // 断线自动重连（Connector 退避）
    m_client->setConnectionCallback([this](const TCPConnection::ptr& c) { onConnection(c); });
    m_client->setMessageCallback([this](const TCPConnection::ptr& c, Buffer* b) { onMessage(c, b); });
    if (m_cfg.tls.enabled) {
        // https：装配期建上下文 + decorator 给每条新连接 attach 客户端会话
        // （U3 装配挂点）；SNI/主机名校验用裸主机名（IP 走 SAN IP 匹配——R12）
        m_tls_ctx = ssl::SslContext::createClient();
        if (m_cfg.tls.verifyPeer) {
            m_tls_ctx->useCaFile(m_cfg.tls.caFile);
        } else {
            m_tls_ctx->disableVerify();
        }
        const std::string host = HostPartOf(addr);
        m_client->setNewConnectionDecorator([this, host](const TCPConnection::ptr& conn) {
            auto session = std::make_shared<ssl::SslSession>(ssl::Role::kClient, *m_tls_ctx);
            session->setPeerName(host);
            conn->attachSsl(std::move(session));
        });
    }
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
    if (m_stopped.load() || m_tls_dead.load()) {
        LOG_WARN(g_logger) << "HttpClient: Request after Stop/TLS-fatal ignored";
        return;
    }
    bool expected = false;
    if (!m_inflight.compare_exchange_strong(expected, true)) {
        LOG_WARN(g_logger) << "HttpClient: request already in flight, rejected";
        return;  // 单在途
    }
    // 统一 park 后投递一次 flush（D54）：连接已就绪则本闭包内即发出；未就绪留在
    // 暂存区等 onConnection。旧实现「调用线程判定就绪→直接投递 / 未就绪→park」，
    // 在断连→重连完成的窗口内 park 的请求无人冲刷（新连接的 connected 事件已过，
    // 永久滞留）——park+flush 双触发闭窗，mutex + ready 标志保 exactly-once
    {
        std::lock_guard<std::mutex> lk(m_handoff_mutex);
        m_handoff_req = std::move(req);
        m_handoff_cb = std::move(cb);
        m_handoff_ready = true;
    }
    m_loop->QueueInLoop([this] { TryFlushHandoff(); });
}

void HttpClient::TryFlushHandoff() {
    TCPConnection::ptr c = m_client->connection();
    if (!c || !c->isConnected()) {
        return;  // 未就绪：留在暂存区，等 onConnection(connected) 冲刷
    }
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
        c->send(m_pending_req.Dump(m_host));
    }
}

void HttpClient::Reset() {
    m_client->disconnect();
}

void HttpClient::onConnection(const TCPConnection::ptr& conn) {
    if (!conn->isConnected()) {
        if (m_cfg.tls.enabled && conn->isSslFatal()) {
            // R6：TLS fatal（证书被拒/协议错误）不会因重连好转——停退避重试，
            // 否则「TCP 通→握手败→重连→再握手败」死循环；新 Request 由 m_tls_dead 即拒
            LOG_ERROR(g_logger) << "HttpClient: TLS fatal (" << conn->name() << "), retry disabled";
            m_client->stop();
            m_tls_dead.store(true);
        }
        if (m_disconnected_observer) {
            m_disconnected_observer();
        }
        // 在途失败；暂存区按契约保留——从未发出的请求留待重连后 flush（D54）。
        // TLS fatal 路径已 stop 重试（无重连前景），须连带收口暂存区
        const bool tls_fatal = m_cfg.tls.enabled && conn->isSslFatal();
        failInflight("connection lost", /*fail_parked=*/tls_fatal);
        cancelResponseTimer();
        return;
    }
    // 连接就绪：冲刷暂存请求（若有——与 Request 的 park 后投递共用同一路径）
    TryFlushHandoff();
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

void HttpClient::failInflight(const char* why, bool fail_parked) {
    ResponseCallback cb;
    ResponseCallback handoff_cb;
    {
        std::lock_guard<std::mutex> lk(m_handoff_mutex);
        if (m_handoff_ready && fail_parked) {
            handoff_cb = std::move(m_handoff_cb);  // 无重连前景：暂存请求一并收口（exactly-once）
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
