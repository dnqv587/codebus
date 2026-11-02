//***********************************************
// name      : WsServer.cc
// brief     : WebSocket 服务端实现
// attention : None
//***********************************************
#include "net/websocket/WsServer.h"

#include <algorithm>

#include "log/Logger.h"
#include "net/EventLoop.h"
#include "net/http/HttpMessage.h"
#include "net/websocket/WsHandshake.h"

namespace bus::ws {

static Logger* g_logger = GET_LOGGER("system");

namespace {

inline int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               TimerManager::ClockType::now().time_since_epoch())
        .count();
}

inline int64_t secToNs(uint32_t s) noexcept {
    return static_cast<int64_t>(s) * 1000'000'000;
}

}  // namespace

WsServer::WsServer(EventLoop* loop, WsServerConfig cfg)
    : m_cfg(cfg),
      m_loop(loop) {
}

WsServer::~WsServer() {
    // 契约：须已 Stop。兜底：未 Stop 且定时器存在——投递清理（loop 线程纪律，D40 同型；
    // loop 已停则 leak 由 ASAN 暴露——契约违约的可见代价优于跨线程析构崩溃）
    if (m_timers) {
        // std::function 须可拷贝——unique_ptr 经 shared_ptr 持有入闭包
        auto timers = std::shared_ptr<TimerManager>(m_timers.release());
        Timer::ptr timer = m_sweep_timer;
        m_sweep_timer.reset();
        auto keeper = std::make_shared<std::shared_ptr<TimerManager>>(std::move(timers));
        m_loop->QueueInLoop([keeper, timer]() {
            if (timer) {
                timer->cancel();
            }
            keeper->reset();  // loop 线程析构（D40）
        });
    }
}

void WsServer::Start() {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) {
        return;
    }
    m_loop->RunInLoop([this] {  // loop 线程惰性建（HttpServer 同型）
        m_timers = std::make_unique<TimerManager>(*m_loop);
        const uint32_t half = std::max(1u, std::min({m_cfg.idlePingSec, m_cfg.pongTimeoutSec,
                                                     m_cfg.closeReplySec}) / 2);
        m_sweep_timer = m_timers->addTimer(
            [this] { sweep(); }, TimerManager::ClockType::now() + std::chrono::seconds(half),
            std::chrono::milliseconds(half * 1000));
    });
}

void WsServer::Stop() {
    if (m_started.exchange(false)) {
        // 闭包自包含（不捕 this——Stop 后对象即可析构；loop 线程清理定时器，D40 纪律）
        auto keeper = std::make_shared<std::shared_ptr<TimerManager>>(m_timers.release());
        Timer::ptr timer = m_sweep_timer;
        m_sweep_timer.reset();
        m_loop->QueueInLoop([keeper, timer]() {
            if (timer) {
                timer->cancel();
            }
            keeper->reset();  // loop 线程析构（D40；const 闭包经 keeper 可变）
        });
    }
}

std::shared_ptr<WsSession> WsServer::findSession(const TCPConnection::ptr& conn) {
    std::lock_guard<std::mutex> lk(m_sessions_mutex);
    auto it = m_sessions.find(std::string(conn->name()));
    return it == m_sessions.end() ? nullptr : it->second->session;
}

void WsServer::handleUpgrade(const TCPConnection::ptr& conn, http::HttpRequest req,
                             std::string_view leftover) {
    std::string reason;
    if (!ValidateUpgradeRequest(req, &reason)) {
        LOG_FMT_WARN(g_logger, "WsServer: bad upgrade request: %s", reason.c_str());
        http::HttpResponse resp;
        resp.SendError(http::HttpStatus::kBAD_REQUEST, reason);
        conn->send(resp.Dump(req));
        conn->shutdown();  // 不接管——close 链走 TcpServer 原路
        return;
    }
    // 接管（U7 契约：连接归 WS 层）——重绑消息回调先于 leftover 处理（H2 验证的安全垫）
    auto session = std::make_shared<WsSession>(m_cfg.session);
    auto entry = std::make_shared<SessionEntry>();
    entry->session = session;
    entry->lastActiveNs = nowNs();
    session->setMessageCallback(m_on_message);
    session->setCloseCallback(m_on_close);  // 透传业务回调（勿覆盖——实测教训）

    conn->setMessageCallback(
        [this](const TCPConnection::ptr& c, Buffer* b) { onWsData(c, b); });
    // 弱引用捕获：连接关闭晚于 WsServer 析构时安全（lock 判活——TSAN UAF 实测教训）
    std::weak_ptr<WsServer> weak_self = shared_from_this();
    conn->addCloseObserver([weak_self, name = std::string(conn->name())] {
        if (auto self = weak_self.lock()) {
            std::lock_guard<std::mutex> lk(self->m_sessions_mutex);
            self->m_sessions.erase(name);
        }
    });
    // 注意：不碰 conn context——HttpServer 的 HttpSession 占用 context（U1），
    // WS 覆盖会造成 write_cb 侧类型混淆（实测 SEGFAULT——HttpServer write_cb 把
    // WsEntry 当 HttpSession pump）。WS 会话走自有注册表（name→entry，mutex）
    {
        std::lock_guard<std::mutex> lk(m_sessions_mutex);
        m_sessions.emplace(std::string(conn->name()), entry);
    }

    // 101 + leftover 喂入（边界后字节可能是早期 WS 帧——H2 教训）
    const std::string accept = ComputeAcceptKey(*req.getHeader("sec-websocket-key"));
    conn->send(Build101Response(accept));
    if (!leftover.empty()) {
        const auto send_fn = [conn](std::string_view out) { conn->send(out); };
        if (session->Consume(leftover.data(), leftover.size(), send_fn) == ConsumeResult::kCloseSent) {
            conn->shutdown();
        }
    }
}

void WsServer::onWsData(const TCPConnection::ptr& conn, Buffer* buf) {
    std::shared_ptr<SessionEntry> entry;
    {
        std::lock_guard<std::mutex> lk(m_sessions_mutex);
        auto it = m_sessions.find(std::string(conn->name()));
        if (it == m_sessions.end()) {
            return;
        }
        entry = it->second;
    }
    entry->lastActiveNs = nowNs();
    const auto send_fn = [conn](std::string_view out) { conn->send(out); };
    const char* base = reinterpret_cast<const char*>(buf->PeekAll().data());
    const size_t total = buf->ReadableSize();
    if (entry->session->Consume(base, total, send_fn) == ConsumeResult::kCloseSent) {
        entry->closeSentNs = nowNs();
        buf->SkipRead(total);
        conn->shutdown();  // close 已发：半关等待对端 TCP 关（sweep 死线兜底硬关）
        return;
    }
    buf->SkipRead(total);
}

void WsServer::sendText(const TCPConnection::ptr& conn, std::string_view text) {
    if (auto s = findSession(conn)) {
        s->sendText(text, [conn](std::string_view out) { conn->send(out); });
    }
}

void WsServer::sendBinary(const TCPConnection::ptr& conn, std::string_view data) {
    if (auto s = findSession(conn)) {
        s->sendBinary(data, [conn](std::string_view out) { conn->send(out); });
    }
}

void WsServer::sweep() {
    const int64_t now = nowNs();
    std::vector<std::shared_ptr<SessionEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_sessions_mutex);
        snapshot.reserve(m_sessions.size());
        for (auto& kv : m_sessions) {
            snapshot.push_back(kv.second);
        }
    }
    for (auto& e : snapshot) {
        const int64_t close_at = e->closeSentNs.load();
        if (close_at != 0) {
            if (now - close_at > secToNs(m_cfg.closeReplySec)) {
                // 关闭握手死线：对端未回 echo——硬关（会话将随 close observer 摘表）
                // 注：sweep 无连接引用——经 SessionEntry 不持 conn，硬关闭入 H4 待议
                // 当前仅记日志（close observer 在 TCP 关闭时清理——HttpServer 空闲超时兜底）
                LOG_WARN(g_logger) << "WsServer: close reply timeout (relying on idle sweep)";
            }
            continue;
        }
        const int64_t ping_at = e->pingSentNs.load();
        if (ping_at != 0) {
            if (now - ping_at > secToNs(m_cfg.pongTimeoutSec)) {
                e->pingSentNs = 0;
                // pong 死线：置关闭（下一轮 sweep 处理 close 死线）——会话引用在 snapshot，
                // 连接引用需经注册表扩展；当前由消息层 close 完成（保守：只记日志）
                LOG_WARN(g_logger) << "WsServer: pong timeout";
            }
            continue;
        }
        if (now - e->lastActiveNs.load() > secToNs(m_cfg.idlePingSec)) {
            e->pingSentNs = now;
            // 发 ping 需连接引用：SessionEntry 不持（防环）——经弱引用表由 H4 优化；
            // 当前模型：业务侧 sendText/ping 驱动心跳（文档注明——服务端主动 ping 登记简化）
        }
    }
}

}  // namespace bus::ws
