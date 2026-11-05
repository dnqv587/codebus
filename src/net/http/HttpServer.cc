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

#include <unistd.h>

#include "log/Logger.h"
#include "net/EventLoop.h"
#include "net/http/HttpCompression.h"
#include "net/http/HttpRequestParser.h"
#include "thread/ThreadPool.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");
static Logger* g_access_logger = GET_LOGGER("http.access");

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
    /// @brief provider 流式在途（收尾前保持 true——单飞语义延伸）
    bool providerStreaming = false;
    /// @brief 流帧格式（true=chunked 编码；false=1.0 close-delimited 裸段——U3 实测教训：
    ///        TE 头是 DumpHead 动态产物不落 HeaderMap，帧格式判定须显式记录）
    bool providerChunked = false;
    /// @brief 响应终结后需关连接（close_after 在流式下延迟到流尽——U3：1.0 半关截断流）
    bool closeAfter = false;
    /// @brief 精确字节记账（头+体+终止块；Q41：AccessLog bytes 实值）
    size_t bytesSent = 0;
    /// @brief 流式响应持有（provider 场景 write_cb 间存续；U3）
    std::shared_ptr<HttpResponse> streamingResp;
    /// @brief worker 单飞标志（同连接至多一个在途响应；V6）
    ///        单飞期间字节留连接输入缓冲（U1：天然背压——积压压缩可写区使 readv 减速）
    std::atomic<bool> responsePending{false};
    /// @brief 本连接已处理请求数（keep-alive 上限判定）
    uint64_t requestCount = 0;
};

HttpServer::HttpServer(EventLoop* loop, std::string_view addr, uint32_t thread_num, HttpServerConfig cfg)
    : m_cfg(std::move(cfg)),
      m_loop(loop),
      m_dispatcher(std::make_shared<ServletDispatcher>()),
      m_server(std::make_unique<TcpServer>(loop, addr, thread_num, cfg.maxConnections)) {
    m_server->setConnectionCallback([this](const TCPConnection::ptr& c) { onConnection(c); });
    m_server->setMessageCallback([this](const TCPConnection::ptr& c, Buffer* b) { onMessage(c, b); });
    // 慢客户端+大响应保护：输出缓冲触高水位即 WARN+关（默认 64MB）
    m_server->setWriteCompleteCallback([this](const TCPConnection::ptr& c) {
        auto sess = findSession(c);
        if (!sess) {
            return;
        }
        if (sess->providerStreaming) {
            pumpProvider(c, sess);  // provider 流驱动（U3 拉模型）
        } else if (sess->streamingResp && !c->isFileSending()) {
            sess->streamingResp.reset();  // 文件源流完（head 写完的回调不误释——文件在途判定）
        }
    });
    m_server->setHighWaterMarkCallback(
        [](const TCPConnection::ptr& c, size_t mark) {
            LOG_WARN(g_logger) << "HttpServer: output buffer high-water " << mark
                               << ", closing slow client";
            c->close();
        },
        m_cfg.maxOutputBufferBytes);
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
    m_dispatcher->freeze();  // T4：启动后注册防护（此后注册 WARN+忽略）
    m_server->Start();
    m_loop->RunInLoop([this]  // loop 线程构造纪律（TcpClient 先例）
                      {
                          m_timers = std::make_unique<TimerManager>(m_loop);
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
        // 便捷注册自动识别模式语法：含 "{...}" 参数段或尾段 "*" 走模式路由（U2）
        if (p.find('{') != std::string::npos || p.ends_with("/*")) {
            m_dispatcher->addPatternServlet(path, router);
        } else {
            m_dispatcher->addExactServlet(path, router);
        }
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

std::shared_ptr<HttpServer::HttpSession> HttpServer::findSession(const TCPConnection::ptr& conn) {
    return std::static_pointer_cast<HttpSession>(conn->getContext());  // 无锁热路径（U1）
}

void HttpServer::onConnection(const TCPConnection::ptr& conn) {
    if (m_conn_observer) {
        m_conn_observer(conn, conn->isConnected());
    }
    if (!conn->isConnected())  // 断开：出表（纯数据对象，mutex 下析构安全）
    {
        std::lock_guard<std::mutex> lk(m_sessions_mutex);
        m_sessions.erase(std::string(conn->name()));
        conn->setContext(nullptr);  // 双保险（主清在 handleClose 观察链）
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
    conn->setContext(session);  // 热路径直挂（U1）
    std::lock_guard<std::mutex> lk(m_sessions_mutex);
    m_sessions.emplace(std::string(conn->name()), session);  // sweep 快照表（保留）
}

void HttpServer::onMessage(const TCPConnection::ptr& conn, Buffer* buf) {
    auto session = findSession(conn);
    if (!session) {
        return;  // 会话已被摘除（扫描器判死等），忽略本包
    }
    session->lastActiveNs = nowNs();

    if (session->responsePending.load())  // 单飞中：字节留连接缓冲（U1：天然背压）+ 积压上限
    {
        if (buf->ReadableSize() > m_cfg.maxBodyBytes) {
            LOG_FMT_WARN(g_logger, "HttpServer: input backlog %zu on %s, closing", buf->ReadableSize(),
                         std::string(conn->name()).c_str());
            sendSimple(conn, nullptr, HttpStatus::kPAYLOAD_TOO_LARGE, "pending input overflow");
            shutdownSoon(conn, session);
        }
        return;
    }

    const char* base = reinterpret_cast<const char*>(buf->PeekAll().data());
    size_t total = buf->ReadableSize();
    size_t consumed = 0;
    processInput(conn, session, base, total, &consumed);
    // 进入单飞且本包尚有剩余：留连接缓冲（U1：不拷贝；单飞分支/回灌自然接手）
    buf->SkipRead(consumed);  // 仅消费已处理字节（剩余留缓冲）
}

void HttpServer::processInputBuffer(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session) {
    Buffer* buf = conn->inputBuffer();
    while (buf->ReadableSize() > 0) {
        auto readable = buf->PeekAll();
        size_t consumed = 0;
        processInput(conn, session, reinterpret_cast<const char*>(readable.data()), readable.size(), &consumed);
        buf->SkipRead(consumed);
        if (session->responsePending.load()) {
            return;  // 又进单飞：剩余留缓冲（回灌/EPOLLIN 再续）
        }
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
                if (m_upgrade_handler) {
                    // 先摘会话出表（脱离 408/空闲/半关管理——移交后连接归回调方）
                    {
                        std::lock_guard<std::mutex> lk(m_sessions_mutex);
                        m_sessions.erase(std::string(conn->name()));
                    }
                    // 精确透出边界后字节（早期 WS 帧；一期此处被整体丢弃）
                    // 注意：off 已在循环头部累计 r.consumed（切莫重复相加——下溢）
                    const std::string_view leftover(data + off, len - off);
                    try {  // 升级处理器异常兜底（否则穿透 IO 线程 terminate，H2 审查 P2）
                        m_upgrade_handler(conn, session->parser.TakeRequest(), leftover);
                    } catch (const std::exception& e) {
                        LOG_FMT_ERROR(g_logger, "HttpServer: upgrade handler exception: %s", e.what());
                        sendSimple(conn, nullptr, HttpStatus::kINTERNAL_SERVER_ERROR, "upgrade failed");
                        shutdownSoon(conn, nullptr);
                    } catch (...) {
                        sendSimple(conn, nullptr, HttpStatus::kINTERNAL_SERVER_ERROR, "upgrade failed");
                        shutdownSoon(conn, nullptr);
                    }
                } else {
                    sendSimple(conn, nullptr, HttpStatus::kNOT_IMPLEMENTED,
                               "protocol upgrade not supported");
                    shutdownSoon(conn, session);
                }
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

bool HttpServer::shouldGzip(const HttpResponse& resp, const HttpRequest& req) const {
    if (!m_cfg.gzip.enabled) {
        return false;
    }
    auto ae = req.getHeader("accept-encoding");
    if (!ae.has_value() || ae->find("gzip") == std::string_view::npos) {
        return false;  // 客户端不接受
    }
    if (resp.bodyFileFd() >= 0 || resp.getBody().size() < m_cfg.gzip.minSize) {
        return false;  // 文件源（H4 与流式协同）/过小
    }
    const int code = static_cast<int>(resp.getStatus());
    if (code == 204 || code == 304 || (code >= 100 && code < 200)) {
        return false;  // 无 body 状态
    }
    if (resp.getHeader("content-encoding").has_value()) {
        return false;  // 已有编码
    }
    auto ct = resp.getHeader("content-type");
    return ct.has_value() && IsCompressibleMime(*ct);
}

void HttpServer::sendResponse(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session,
                              const HttpResponse& resp, const HttpRequest& req) {
    // gzip 响应压缩判定链（U2）：内存 body 且全部条件命中才压（判定纯函数化便于测试）
    if (shouldGzip(resp, req)) {
        std::string packed;
        if (GzipCompress(resp.getBody(), packed, m_cfg.gzip.level)) {
            const_cast<HttpResponse&>(resp).setBody(std::move(packed));
            const_cast<HttpResponse&>(resp).setHeader("Content-Encoding", "gzip");
            const_cast<HttpResponse&>(resp).setHeader("Vary", "Accept-Encoding");
        } else {
            LOG_WARN(g_logger) << "HttpServer: gzip compress failed, sending raw";
        }
    }
    // Q41：精确字节记账（头先行记入；体/终止块由各路径累计）
    const std::string head = resp.DumpHead(req);
    session->bytesSent += head.size();
    conn->send(head);
    // HEAD/1xx/204/304 不携带 body（RFC 7231 §4.3.2；Dump 的 omit 语义在此延续——
    // 拆分时丢失该判定是 H2 审查 P0 回归）
    const int code = static_cast<int>(resp.getStatus());
    const bool omit_body =
        (req.getMethod() == HttpMethod::kHEAD) || (code >= 100 && code < 200) || code == 204 || code == 304;
    if (omit_body) {
        return;
    }
    if (resp.hasBodyProvider()) {  // kProvider：置流态，首段立即泵（write_cb 续驱）
        session->providerStreaming = true;
        session->closeAfter = !req.isKeepAlive() || !resp.isKeepAlive();  // 流尽再关（防半关截断）
        session->providerChunked = !(req.getHttpMajor() == 1 && req.getHttpMinor() == 0);  // 1.0 裸段
        session->streamingResp = std::make_shared<HttpResponse>(std::move(const_cast<HttpResponse&>(resp)));
        pumpProvider(conn, session);
        return;
    }
    if (resp.bodyFileFd() >= 0) {
        // kFile：sendfile(2) 零拷贝（U3；TLS 通道 H4 走 pread 降级链）。
        // resp 移入 session 保活：fd 所有权在 resp（shared_ptr<int>），栈析构=关 fd
        // → pump 时 EBADF（实测教训）；流完 write_cb / 会话析构自然释放
        session->streamingResp = std::make_shared<HttpResponse>(
            std::move(const_cast<HttpResponse&>(resp)));
        const HttpResponse& kept = *session->streamingResp;
        session->bytesSent += kept.bodyFileSize();
        conn->sendFile(kept.bodyFileFd(), kept.bodyFileOffset(), kept.bodyFileSize());
        return;
    }
    if (!resp.getBody().empty()) {
        session->bytesSent += resp.getBody().size();
        conn->send(resp.getBody());
    }
}

void HttpServer::sendResponseRaw(const TCPConnection::ptr& conn, const HttpResponse& resp,
                                 const HttpRequest& req) {
    // 会话已失时的退化路径：无记账无流态（正常请求生命周期不应到达——防御分支）
    conn->send(resp.DumpHead(req));
    if (!resp.getBody().empty()) {
        conn->send(resp.getBody());
    }
}

void HttpServer::pumpProvider(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session) {
    // chunked/close-delimited 拉模型（U3）：拉一段-编码-send；write_cb 逐段续驱；
    // HTTP/1.0 无编码直接体，流尽后由 close_after 关闭（framing 靠 EOF）
    if (!session->streamingResp) {
        LOG_ERROR(g_logger) << "pumpProvider: streamingResp null, streaming=" << session->providerStreaming;
        session->providerStreaming = false;
        return;
    }
    HttpResponse& resp = *session->streamingResp;
    const bool chunked = session->providerChunked;  // 帧格式在 sendResponse 显式记录
    std::string chunk;
    try {
        while (true) {
            chunk.clear();
            if (!resp.bodyProvider()(chunk)) {
                break;  // 流尽
            }
            if (chunk.empty()) {
                continue;  // 空段跳过（provider 想让出）
            }
            if (chunked) {
                // RFC 9112 §7.1：chunk-size 行为十六进制
                char szbuf[20];
                snprintf(szbuf, sizeof(szbuf), "%zx", chunk.size());
                std::string frame = szbuf + ("\r\n" + chunk + "\r\n");
                session->bytesSent += frame.size();
                conn->send(frame);
            } else {
                session->bytesSent += chunk.size();
                conn->send(chunk);
            }
            return;  // 等本段写完的 write_cb 再拉下一段（拉模型：单在途一段）
        }
        // 流尽收尾
        if (chunked) {
            const std::string term = "0\r\n\r\n";
            session->bytesSent += term.size();
            conn->send(term);
        }
        session->providerStreaming = false;
        session->streamingResp.reset();
        session->responsePending = false;  // 单飞收尾
        // 回灌：流期间到达的字节
        if (conn->inputBuffer()->ReadableSize() > 0) {
            processInputBuffer(conn, session);
        }
        if (session->closeAfter) {  // 延迟关闭兑现（1.0 close-delimited 流尽即关）
            session->closeAfter = false;
            shutdownSoon(conn, session);
        }
    } catch (const std::exception& e) {
        LOG_FMT_ERROR(g_logger, "HttpServer: provider exception: %s", e.what());
        session->providerStreaming = false;
        session->streamingResp.reset();
        session->responsePending = false;
        conn->shutdown();  // framing 不可信不保活（Q40：记 shutdownNs 走兜底）
        if (session) {
            session->shutdownNs = nowNs();
        }
    }
}

void HttpServer::dispatch(const TCPConnection::ptr& conn, const std::shared_ptr<HttpSession>& session,
                          HttpRequest req) {
    // HTTP/1.1 缺 Host 头 → 400（RFC 7230 §5.4 强制）
    if (req.getHttpMajor() == 1 && req.getHttpMinor() >= 1 && !req.getHeader("host").has_value()) {
        HttpResponse resp(m_cfg.serverName);
        resp.SendError(HttpStatus::kBAD_REQUEST, "missing Host header");
        conn->send(resp.Dump(req));
        shutdownSoon(conn, session);
        return;
    }
    // 请求体解压（U2：Content-Encoding: gzip；上限=maxBodyBytes——zlib bomb 防护）
    if (m_cfg.requestGunzip) {
        auto ce = req.getHeader("content-encoding");
        if (ce.has_value() && *ce == "gzip" && !req.getBody().empty()) {
            std::string plain;
            const int r = GzipDecompress(req.getBody(), plain, m_cfg.maxBodyBytes);
            if (r == 0) {
                req.setBodyDecompressed(std::move(plain));
            } else {
                const HttpStatus st = (r == -2) ? HttpStatus::kPAYLOAD_TOO_LARGE : HttpStatus::kBAD_REQUEST;
                sendSimple(conn, &req, st, r == -2 ? "decompressed body too large" : "bad gzip body");
                shutdownSoon(conn, session);
                return;
            }
        }
    }
    req.setRemoteAddr(std::string(conn->remoteAddr().toString()));  // AccessLog/业务可用
    const int64_t t_start = nowNs();
    // keep-alive 请求数上限：达限本响应后关连接（nginx keepalive_requests 语义）
    const bool limit_reached = (++session->requestCount >= m_cfg.keepAliveRequests);
    if (m_pool)  // worker 模式：投递业务池（handler 可阻塞不占 IO 线程）
    {
        session->responsePending = true;  // 单飞：同连接至多一个在途响应（保序）
        EventLoop* io_loop = conn->getLoop();
        std::weak_ptr<TCPConnection> weak_conn = conn;
        std::weak_ptr<HttpSession> weak_session = session;
        m_pool->run([this, weak_conn, weak_session, limit_reached, t_start, req = std::move(req), io_loop]() mutable {
            HttpResponse resp(m_cfg.serverName);
            m_dispatcher->Handle(req, resp);
            if (limit_reached) {
                resp.setKeepAlive(false);
            }
            bool close_after = !req.isKeepAlive() || !resp.isKeepAlive();
            if (auto c = weak_conn.lock()) {
                auto ws = weak_session.lock();  // 供 sendResponse 记账/流态（Q41）
                if (ws) {
                    sendResponse(c, ws, resp, req);
                } else {
                    sendResponseRaw(c, resp, req);  // 会话已失：退化为无记账发送
                }
                const size_t bytes = ws ? ws->bytesSent : 0;  // 精确值（Q41；无会话记 0）
                if (m_cfg.accessLog) {
                    logAccess(req, resp, nowNs() - t_start, bytes);
                }
                ws ? ws->bytesSent = 0 : 0;  // 记账复位（下一请求重新累计）
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
                    if (c->inputBuffer()->ReadableSize() > 0)  // 回灌：单飞期间到达的字节（U1：直连缓冲）
                    {
                        processInputBuffer(c, s);
                    }
                });
        });
        return;
    }

    // IO 内联路径（低延迟默认）
    HttpResponse resp(m_cfg.serverName);
    m_dispatcher->Handle(req, resp);
    if (limit_reached) {
        resp.setKeepAlive(false);
    }
    sendResponse(conn, session, resp, req);
    if (m_cfg.accessLog) {
        logAccess(req, resp, nowNs() - t_start, session->bytesSent);  // Q41 精确值
    }
    session->bytesSent = 0;  // 记账复位
    if ((!req.isKeepAlive() || !resp.isKeepAlive()) && !session->providerStreaming) {
        shutdownSoon(conn, session);  // 流在途由流尽收尾补关（closeAfter 已记）
    }
}

void HttpServer::logAccess(const HttpRequest& req, const HttpResponse& resp, int64_t dur_ns,
                           size_t bytes) {
    // NCSA combined + 扩展：addr - - [ts] "METHOD path VER" status bytes "ref" "ua" dur_ms
    auto referer = req.getHeader("referer");
    auto ua = req.getHeader("user-agent");
    LOG_INFO(g_access_logger) << req.getRemoteAddr() << " - - "
                              << "\"" << HttpMethodToString(req.getMethod()) << ' ' << req.getPath()
                              << ' ' << req.getVersion() << "\" " << static_cast<int>(resp.getStatus())
                              << ' ' << bytes << " \"" << referer.value_or("-") << "\" \""
                              << ua.value_or("-") << "\" " << dur_ns / 1'000'000;
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
