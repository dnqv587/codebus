//***********************************************
// name      : WsClient.cc
// brief     : WebSocket 客户端实现（握手 + 帧泵 + 关闭收口）
// attention : 全部状态 loop 线程独占；stop 经 QueueInLoop（TimerManager/连接清理纪律）
//***********************************************
#include "net/websocket/WsClient.h"

#include "log/Logger.h"
#include "net/EventLoop.h"
#include "net/websocket/WsHandshake.h"

namespace bus::ws {

static Logger* g_logger = GET_LOGGER("sys");

WsClient::WsClient(EventLoop* loop, std::string_view addr, WsClientConfig cfg)
    : m_loop(loop),
      m_host(addr),
      m_cfg(cfg),
      m_client(std::make_unique<TcpClient>(loop, addr, "WsClient")),
      m_session([cfg]() {
          WsSessionConfig sc = cfg.session;
          sc.client = true;  // 客户端视角强制（收帧须不掩码、发帧必掩码）
          return sc;
      }()) {
    m_client->setConnectionCallback([this](const TCPConnection::ptr& c) { onConnection(c); });
    m_client->setMessageCallback([this](const TCPConnection::ptr& c, Buffer* b) { onMessage(c, b); });
    m_session.setMessageCallback([this](const WsMessage& m) {
        if (m_on_message) {
            m_on_message(m);
        }
    });
    m_session.setCloseCallback([this](uint16_t code, std::string_view reason) {
        // close 握手收口（对端 close 或 echo 到达）：交付一次 + 客户端关 TCP（RFC §5.5.1）
        m_close_delivered = true;
        if (m_on_close) {
            m_on_close(code, reason);
        }
        if (auto c = m_client->connection()) {
            c->close();
        }
    });
    m_session.setPongCallback([this] {
        if (m_on_pong) {
            m_on_pong();
        }
    });
}

WsClient::~WsClient() noexcept {
    // 契约：须已 stop 且连接断开（TcpClient 析构断言兜底）
}

void WsClient::connect(std::string_view path) {
    // 状态 loop 线程独占：路径装配与 connect 一并串行化（审查修复）
    m_loop->QueueInLoop([this, p = std::string(path)] {
        m_path = p.empty() ? "/" : p;
        m_client->connect();
    });
}

void WsClient::stop() {
    m_loop->QueueInLoop([this] {
        m_client->stop();
        m_client->disconnect();
    });
}

std::function<void(std::string_view)> WsClient::sendFn() {
    return [this](std::string_view d) {
        if (auto c = m_client->connection()) {
            c->send(d);
        }
    };
}

void WsClient::sendText(std::string_view text) {
    // 会话 loop 亲和：跨线程入口经 QueueInLoop 串行化（HttpClient Request 同纪律）
    m_loop->QueueInLoop([this, t = std::string(text)] {
        if (!m_open) {
            LOG_WARN(g_logger) << "WsClient: sendText before open ignored";
            return;
        }
        m_session.sendText(t, sendFn());
    });
}

void WsClient::sendBinary(std::string_view data) {
    m_loop->QueueInLoop([this, d = std::string(data)] {
        if (!m_open) {
            LOG_WARN(g_logger) << "WsClient: sendBinary before open ignored";
            return;
        }
        m_session.sendBinary(d, sendFn());
    });
}

void WsClient::ping() {
    m_loop->QueueInLoop([this] {
        if (!m_open) {
            LOG_WARN(g_logger) << "WsClient: ping before open ignored";
            return;
        }
        m_session.ping(sendFn());
    });
}

void WsClient::close(uint16_t code, std::string_view reason) {
    m_loop->QueueInLoop([this, code, r = std::string(reason)] {
        if (!m_open) {
            return;
        }
        m_session.close(code, r, sendFn());
    });
}

void WsClient::failHandshake(const TCPConnection::ptr& conn, std::string_view reason) {
    if (m_handshake_failed) {
        return;
    }
    m_handshake_failed = true;
    LOG_WARN(g_logger) << "WsClient: handshake failed: " << reason;
    if (m_on_error) {
        m_on_error(reason);
    }
    conn->close();
}

void WsClient::onConnection(const TCPConnection::ptr& conn) {
    if (conn->isConnected()) {
        // 连接就绪（loop 线程）：装配握手解析器 + 发升级请求
        m_key = GenerateSecKey();
        m_parser.start(bus::http::HttpMethod::kGET);
        conn->send(BuildUpgradeRequest(m_path, m_host, m_key));
        return;
    }
    // 断连收口
    if (!m_open) {
        if (!m_handshake_failed) {
            m_handshake_failed = true;
            if (m_on_error) {
                m_on_error("connection lost during handshake");
            }
        }
        return;
    }
    if (!m_close_delivered) {
        m_close_delivered = true;  // 异常断连（无 close 握手）→ 1006
        if (m_on_close) {
            m_on_close(1006, "");
        }
    }
}

void WsClient::onMessage(const TCPConnection::ptr& conn, Buffer* buf) {
    if (!m_open) {
        // 握手阶段：解析 101 响应
        while (buf->ReadableSize() > 0) {
            auto readable = buf->PeekAll();
            bus::http::ParseResult r =
                m_parser.Execute(reinterpret_cast<const char*>(readable.data()), readable.size());
            buf->SkipRead(r.consumed);
            if (r.event == bus::http::ParseEvent::kUpgrade) {
                bus::http::HttpResponse resp = m_parser.takeResponse();
                std::string reason;
                if (!Validate101Response(resp, m_key, &reason)) {
                    failHandshake(conn, reason);
                    return;
                }
                m_open = true;
                if (m_on_open) {
                    m_on_open();
                }
                // leftover（early-frame）喂会话
                if (buf->ReadableSize() > 0) {
                    auto rest = buf->PeekAll();
                    const size_t n = rest.size();
                    m_session.Consume(reinterpret_cast<const char*>(rest.data()), n, sendFn());
                    buf->SkipRead(n);
                }
                return;
            }
            if (r.event == bus::http::ParseEvent::kComplete) {
                failHandshake(conn, "expected 101, got complete response");
                return;
            }
            if (r.event == bus::http::ParseEvent::kError) {
                failHandshake(conn, "handshake response parse error");
                return;
            }
            return;  // kNeedMore
        }
        return;
    }
    // 帧阶段：会话消费（掩码校验/重组/控制帧）
    auto readable = buf->PeekAll();
    const size_t n = readable.size();
    const ConsumeResult cr = m_session.Consume(reinterpret_cast<const char*>(readable.data()), n, sendFn());
    buf->SkipRead(n);
    // kCloseSent 且未收口：对端违规已回 close，等 echo；对端无响应时由 stop/对端关连接收口（v1 无 sweep）
    (void)cr;
}

}  // namespace bus::ws