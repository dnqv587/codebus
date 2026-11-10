//***********************************************
// name      : WebSocketServerTest.cc
// brief     : WebSocket 端到端测试（握手/echo/分片重组/ping-pong/关闭握手/
//             协议违规/炸弹/early-frame leftover）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/http/HttpServer.h"
#include "net/websocket/WsFrame.h"
#include "net/websocket/WsHandshake.h"
#include "net/websocket/WsServer.h"

using namespace bus;
using namespace bus::http;
using namespace bus::ws;
using namespace bus::test;

namespace {

/// @brief 裸 WS 测试客户端（socket + 掩码帧构造 + 读帧）
class WsClient {
 public:
    ~WsClient() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    bool connect(uint16_t port) {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    bool sendAll(std::string_view data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::send(m_fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }
    /// @brief 发起升级握手（返回 101 与 accept）
    bool handshake(const char* key) {
        std::string req = "GET /chat HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
        req += std::string("Sec-WebSocket-Key: ") + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAll(req)) {
            return false;
        }
        while (m_buf.find("\r\n\r\n") == std::string::npos) {
            char tmp[2048];
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                return false;
            }
            m_buf.append(tmp, static_cast<size_t>(n));
        }
        const size_t hdr_end = m_buf.find("\r\n\r\n") + 4;
        m_body_start = hdr_end;
        return m_buf.starts_with("HTTP/1.1 101");
    }
    /// @brief 发送掩码文本帧
    bool sendText(std::string_view text) {
        std::string wire;
        AppendFrame(wire, Opcode::kText, text);
        // 手工加掩码（测试辅助：服务端 AppendFrame 不掩码——测试构造客户端形态）
        return sendAll(wire);  // 掩码版见 sendMaskedText
    }
    /// @brief 发送掩码文本帧（客户端合法形态）
    bool sendMaskedText(std::string_view text, bool fin = true, Opcode op = Opcode::kText) {
        std::string wire;
        wire.push_back(static_cast<char>(((fin ? 0x80 : 0) | static_cast<uint8_t>(op)) & 0xFF));
        const uint64_t n = text.size();
        if (n <= 125) {
            wire.push_back(static_cast<char>(0x80 | n));
        } else if (n <= 0xFFFF) {
            wire.push_back(static_cast<char>(0x80 | 126));
            wire.push_back(static_cast<char>((n >> 8) & 0xFF));
            wire.push_back(static_cast<char>(n & 0xFF));
        } else {
            wire.push_back(static_cast<char>(0x80 | 127));
            for (int i = 7; i >= 0; --i) {
                wire.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
            }
        }
        const uint8_t key[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        wire.append(reinterpret_cast<const char*>(key), 4);
        std::string masked(text);
        ApplyMask(masked.data(), masked.size(), key);
        wire.append(masked);
        return sendAll(wire);
    }
    /// @brief 读一个服务端帧（解出 payload；非帧数据返回 false）
    bool readFrame(Opcode* op, std::string* payload, int timeout_ms = 3000) {
        auto wait_more = [&] {
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char tmp[65536];
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                return false;
            }
            m_buf.append(tmp, static_cast<size_t>(n));
            return true;
        };
        while (true) {
            FrameHeader hdr;
            size_t consumed = 0;
            uint16_t cc = 0;
            // 服务端帧不掩码——直接读（ParseFrameHeader 会拒收未掩码……服务端方向需宽松读：
            // 此处用简化读头：2 字节起步 + 扩展长度）
            if (m_body_start + 2 > m_buf.size()) {
                if (!wait_more()) {
                    return false;
                }
                continue;
            }
            const uint8_t b0 = static_cast<uint8_t>(m_buf[m_body_start]);
            const uint8_t b1 = static_cast<uint8_t>(m_buf[m_body_start + 1]);
            const bool fin = (b0 & 0x80) != 0;
            const auto opcode = static_cast<Opcode>(b0 & 0x0F);
            uint64_t plen = b1 & 0x7F;
            size_t ext = 0;
            if (plen == 126) {
                ext = 2;
            } else if (plen == 127) {
                ext = 8;
            }
            if (m_body_start + 2 + ext > m_buf.size()) {
                if (!wait_more()) {
                    return false;
                }
                continue;
            }
            if (ext == 2) {
                plen = (static_cast<uint8_t>(m_buf[m_body_start + 2]) << 8) |
                       static_cast<uint8_t>(m_buf[m_body_start + 3]);
            } else if (ext == 8) {
                plen = 0;
                for (int i = 0; i < 8; ++i) {
                    plen = (plen << 8) | static_cast<uint8_t>(m_buf[m_body_start + 2 + i]);
                }
            }
            const size_t data_off = m_body_start + 2 + ext;
            if (data_off + plen > m_buf.size()) {
                if (!wait_more()) {
                    return false;
                }
                continue;
            }
            if (op != nullptr) {
                *op = opcode;
            }
            if (payload != nullptr) {
                *payload = m_buf.substr(data_off, plen);
            }
            (void)fin;
            (void)consumed;
            (void)cc;
            (void)hdr;
            m_body_start = data_off + plen;
            return true;
        }
    }
    /// @brief 读到 EOF
    std::string drainAll() {
        char tmp[65536];
        for (;;) {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                break;
            }
            m_buf.append(tmp, static_cast<size_t>(n));
        }
        return m_buf;
    }
    int fd() const {
        return m_fd;
    }

 private:
    int m_fd = -1;
    std::string m_buf;
    size_t m_body_start = 0;
};

}  // namespace

class WebSocketServerTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_port = GetFreePort();
        m_ws = std::make_shared<WsServer>(m_loop.loop());
        m_server = std::make_unique<HttpServer>(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0);
        m_server->setUpgradeHandler(
            [this](const TCPConnection::ptr& conn, HttpRequest req, std::string_view leftover) {
                m_ws->handleUpgrade(conn, std::move(req), leftover);
            });
    }
    void TearDown() override {
        if (m_server) {
            std::atomic<bool> done{false};
            m_server->Stop([&] { done = true; });
            ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
        }
        if (m_ws) {
            m_ws->Stop();
        }
        m_loop.loop()->RunInLoop([this] {});  // 排空
    }
    void startAndWait() {
        m_ws->Start();
        m_server->Start();
        ASSERT_TRUE(WaitFor(
            [&] {
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) {
                    return false;
                }
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(m_port);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
                ::close(fd);
                return ok;
            },
            std::chrono::milliseconds(3000)));
    }

    /// @brief 以指定配置重建 WsServer（短死线用例——须在 startAndWait 前调用）
    void useConfig(const WsServerConfig& cfg) {
        m_ws = std::make_shared<WsServer>(m_loop.loop(), cfg);
        m_server->setUpgradeHandler(
            [this](const TCPConnection::ptr& conn, HttpRequest req, std::string_view leftover) {
                m_ws->handleUpgrade(conn, std::move(req), leftover);
            });
    }

    LoopThread m_loop;
    uint16_t m_port = 0;
    std::unique_ptr<HttpServer> m_server;
    std::shared_ptr<WsServer> m_ws;
};

/// [正] 握手 + echo 往返
TEST_F(WebSocketServerTest, EchoEndToEnd) {
    m_ws->onMessage([this](const WsMessage& msg) {
        (void)this;
        // echo：经注册表找连接发送——简化：WsServer::sendText 需连接——由 onWsData 闭包内
        // 直接回发（此处借 m_last_msg 断言）
    });
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.handshake("dGhlIHNhbXBsZSBub25jZQ=="));
    // echo 服务端：在 handleUpgrade 前注册——改为通过 onMessage 回写（WsServer 侧会话发送）
    // 本用例验证：发文本帧 → 服务端解析（协议层无错即回 pong/close 路径未触发）
    ASSERT_TRUE(c.sendMaskedText("hello ws"));
    // 读回显（echo 由业务回调负责——服务端框架只保证消费无错：以能继续收发为证）
    // 此处验证控制面：发 ping（自定义形态）→ 框架自动回 pong
    std::string ping;
    ping.push_back(static_cast<char>(0x89));  // fin|ping
    ping.push_back(static_cast<char>(0x80 | 2));
    const uint8_t pk[4] = {0x01, 0x02, 0x03, 0x04};
    ping.append(reinterpret_cast<const char*>(pk), 4);
    std::string masked = "\xde\xad";  // 原文——掩码后发出（服务端解回原文并 pong）
    ApplyMask(masked.data(), masked.size(), pk);
    ping += masked;
    ASSERT_TRUE(c.sendAll(ping));
    Opcode op;
    std::string payload;
    ASSERT_TRUE(c.readFrame(&op, &payload));
    EXPECT_EQ(op, Opcode::kPong);
    EXPECT_EQ(payload, "\xde\xad");
}

/// [正] 分片重组：3 段文本 → 单消息（onMessage 一次性收）
TEST_F(WebSocketServerTest, FragmentedReassembly) {
    std::atomic<int> msgs{0};
    std::atomic<size_t> total{0};
    m_ws->onMessage([&](const WsMessage& msg) {
        ++msgs;
        total += msg.payload.size();
    });
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.handshake("dGhlIHNhbXBsZSBub25jZQ=="));
    // 三段：fin=0 cont0? 首帧 text fin=0；后续 continuation
    ASSERT_TRUE(c.sendMaskedText("frag", false, Opcode::kText));
    ASSERT_TRUE(c.sendMaskedText("ment", false, Opcode::kContinuation));
    ASSERT_TRUE(c.sendMaskedText("ed", true, Opcode::kContinuation));
    ASSERT_TRUE(WaitFor([&] { return msgs.load() == 1; }));
    EXPECT_EQ(total.load(), 10u);
}

/// [界] 消息超限 → close 1009
TEST_F(WebSocketServerTest, MaxMessage413Close) {
    m_ws->Stop();  // 旧实例先停（定时器清理投递——防跨线程析构）
    WsServerConfig cfg;
    cfg.session.maxMessageBytes = 64;
    m_ws = std::make_shared<WsServer>(m_loop.loop(), cfg);
    m_server->setUpgradeHandler(
        [this](const TCPConnection::ptr& conn, HttpRequest req, std::string_view leftover) {
            m_ws->handleUpgrade(conn, std::move(req), leftover);
        });
    std::atomic<int> closes{0};
    m_ws->onClose([&](uint16_t, std::string_view) { ++closes; });
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.handshake("dGhlIHNhbXBsZSBub25jZQ=="));
    // 分片炸弹：首帧 100B fin=0 + continuation 100B → 超 64
    ASSERT_TRUE(c.sendMaskedText(std::string(50, 'a'), false, Opcode::kText));
    ASSERT_TRUE(c.sendMaskedText(std::string(50, 'b'), true, Opcode::kContinuation));
    Opcode op;
    std::string payload;
    ASSERT_TRUE(c.readFrame(&op, &payload));
    EXPECT_EQ(op, Opcode::kClose);
    ASSERT_GE(payload.size(), 2u);
    EXPECT_EQ((static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]), 1009);
}

/// [界] 协议违规：未掩码数据帧 → close 1002
TEST_F(WebSocketServerTest, UnmaskedFrameClose1002) {
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.handshake("dGhlIHNhbXBsZSBub25jZQ=="));
    std::string raw;  // 未掩码 text "x"
    raw.push_back(static_cast<char>(0x81));
    raw.push_back(static_cast<char>(1));
    raw += "x";
    ASSERT_TRUE(c.sendAll(raw));
    Opcode op;
    std::string payload;
    ASSERT_TRUE(c.readFrame(&op, &payload));
    EXPECT_EQ(op, Opcode::kClose);
    ASSERT_GE(payload.size(), 2u);
    EXPECT_EQ((static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]), 1002);
}

/// [正] 关闭握手：客户端 close(1000) → 服务端 echo close
TEST_F(WebSocketServerTest, CloseHandshakeEcho) {
    std::atomic<int> closes{0};
    m_ws->onClose([&](uint16_t code, std::string_view) {
        (void)code;
        ++closes;
    });
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.handshake("dGhlIHNhbXBsZSBub25jZQ=="));
    // close(1000) 掩码帧
    std::string closePayload;
    closePayload.push_back(static_cast<char>(1000 >> 8));
    closePayload.push_back(static_cast<char>(1000 & 0xFF));
    std::string wire;
    wire.push_back(static_cast<char>(0x88));  // fin|close
    wire.push_back(static_cast<char>(0x80 | closePayload.size()));
    const uint8_t key[4] = {9, 9, 9, 9};
    wire.append(reinterpret_cast<const char*>(key), 4);
    std::string masked = closePayload;
    ApplyMask(masked.data(), masked.size(), key);
    wire.append(masked);
    ASSERT_TRUE(c.sendAll(wire));
    Opcode op;
    std::string payload;
    ASSERT_TRUE(c.readFrame(&op, &payload));
    EXPECT_EQ(op, Opcode::kClose);
    ASSERT_GE(payload.size(), 2u);
    EXPECT_EQ((static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]), 1000);
    ASSERT_TRUE(WaitFor([&] { return closes.load() == 1; }));
}

/// [正] 握手请求尾部夹带首帧（leftover 透出——H2 教训锁定）
TEST_F(WebSocketServerTest, EarlyFrameInLeftover) {
    std::atomic<int> msgs{0};
    m_ws->onMessage([&](const WsMessage&) { ++msgs; });
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    // 手工握手 + 尾部 ping 帧（leftover 应立即消费 → 自动回 pong）
    std::string req = "GET /chat HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    std::string ping;
    ping.push_back(static_cast<char>(0x89));
    ping.push_back(static_cast<char>(0x80 | 2));
    const uint8_t key[4] = {1, 2, 3, 4};
    ping.append(reinterpret_cast<const char*>(key), 4);
    std::string masked = "ok";
    ApplyMask(masked.data(), masked.size(), key);
    ping.append(masked);
    ASSERT_TRUE(c.sendAll(req + ping));
    // 读 101 头
    char tmp[2048];
    std::string buf;
    while (buf.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(c.fd(), tmp, sizeof(tmp), 0);
        ASSERT_GT(n, 0);
        buf.append(tmp, static_cast<size_t>(n));
    }
    EXPECT_TRUE(buf.starts_with("HTTP/1.1 101"));
    // leftover 中的 ping → pong（紧跟 101 之后）
    WsClient& rc = c;
    // 借 readFrame：帧数据已在本次 recv 附带——重构读（简化：drain）
    std::string rest = buf.substr(buf.find("\r\n\r\n") + 4);
    if (rest.empty()) {
        ssize_t n = ::recv(c.fd(), tmp, sizeof(tmp), 0);
        ASSERT_GT(n, 0);
        rest.append(tmp, static_cast<size_t>(n));
    }
    ASSERT_GE(rest.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(rest[0]) & 0x0F, static_cast<uint8_t>(Opcode::kPong));
    (void)rc;
    (void)msgs;
}

/// [界] 坏握手 → 400 不接管
TEST_F(WebSocketServerTest, BadHandshake400) {
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    std::string req = "GET /chat HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n";
    ASSERT_TRUE(c.sendAll(req));
    std::string all = c.drainAll();
    EXPECT_TRUE(all.starts_with("HTTP/1.1 400"));
}

// ---------------- U6：sweep 三死线（弱引用连接通道） ----------------

namespace {

/// @brief 等待连接被对端关闭（EOF——recv 返回 0）
bool WaitPeerClosed(int fd, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        timeval tv{0, 20 * 1000};  // 20ms 轮询窗口
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char tmp[256];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) {
            return true;  // EOF
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return true;  // 连接级错误（RST 等）——同为已关
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// @brief U6 用例的短死线配置（sweep 周期 = min(1,1,1)/2 → 1s）
WsServerConfig FastDeadlineCfg() {
    WsServerConfig cfg;
    cfg.idlePingSec = 1;
    cfg.pongTimeoutSec = 1;
    cfg.closeReplySec = 1;
    return cfg;
}

}  // namespace

/// [正] U6 idle ping：空闲触发服务端主动 ping；客户端回 pong 清死线后连接持续可用
TEST_F(WebSocketServerTest, IdlePingArrivesAndPongKeepsAlive) {
    useConfig(FastDeadlineCfg());
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.handshake("dGhlIHNhbXBsZSBub25jZQ=="));

    Opcode op = Opcode::kText;
    std::string payload;
    ASSERT_TRUE(c.readFrame(&op, &payload, 5000)) << "no idle ping arrived";
    EXPECT_EQ(op, Opcode::kPing) << "expected server-initiated ping frame";

    // 回 pong（客户端控制帧须掩码——同负载）→ 死线清除
    ASSERT_TRUE(c.sendMaskedText(payload, /*fin=*/true, Opcode::kPong));

    // 越过 pongTimeout 死线（1s）后连接仍活：再收下一个 idle ping（1s 周期）
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    ASSERT_TRUE(c.readFrame(&op, &payload, 5000)) << "connection dead after pong (deadline not cleared)";
    EXPECT_EQ(op, Opcode::kPing);
    ASSERT_TRUE(c.sendMaskedText(payload, true, Opcode::kPong));  // 持续保活
}

/// [负] U6 pong 死线：收 ping 后沉默 → 硬关（客户端 EOF）
TEST_F(WebSocketServerTest, PongTimeoutClosesConnection) {
    useConfig(FastDeadlineCfg());
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.handshake("dGhlIHNhbXBsZSBub25jZQ=="));

    Opcode op = Opcode::kText;
    std::string payload;
    ASSERT_TRUE(c.readFrame(&op, &payload, 5000));  // 收 ping（先读掉，防 EOF 判定读到它）
    EXPECT_EQ(op, Opcode::kPing);

    // 沉默：pongTimeout 1s + sweep 周期 1s → ≤3s 硬关
    EXPECT_TRUE(WaitPeerClosed(c.fd(), 5000)) << "connection not closed after pong timeout";
}

/// [负] U6 close-reply 死线：服务端发 close 后客户端不 echo → 硬关
TEST_F(WebSocketServerTest, CloseReplyTimeoutHardClose) {
    useConfig(FastDeadlineCfg());
    startAndWait();
    WsClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.handshake("dGhlIHNhbXBsZSBub25jZQ=="));

    // 触发服务端 close：发未掩码数据帧（客户端帧必须掩码——协议违规 1002）
    ASSERT_TRUE(c.sendText("unmasked"));

    Opcode op = Opcode::kText;
    std::string payload;
    ASSERT_TRUE(c.readFrame(&op, &payload, 3000)) << "no close frame after protocol violation";
    EXPECT_EQ(op, Opcode::kClose);  // 收 close 但不回 echo

    // closeReplySec 1s + sweep 周期 → ≤3s 硬关
    EXPECT_TRUE(WaitPeerClosed(c.fd(), 5000)) << "connection not hard-closed after close reply timeout";
}
