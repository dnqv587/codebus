//***********************************************
// name      : WssServerTest.cc
// brief     : wss 端到端测试（H4-U7）：HttpsServer + UpgradeHandler 组装即得
//             ——决策 D-1/D-3 红利验收（预期零产品代码改动）
// attention : 客户端为「TcpClient + decorator attach TLS 会话 + 自写服务端帧头解析」
//             （ParseFrameHeader 内建服务端规则会拒未掩码帧——服务端帧永不掩码）；
//             证书位于 test/certs/（CODEBUS_TEST_CERT_DIR 定位）
//***********************************************
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Buffer.hpp"
#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/EventLoop.h"
#include "net/TcpClient.h"
#include "net/http/HttpServer.h"
#include "net/ssl/SslContext.h"
#include "net/ssl/SslSession.h"
#include "net/websocket/WsFrame.h"
#include "net/websocket/WsHandshake.h"
#include "net/websocket/WsServer.h"
#include "type.h"

using namespace bus;
using namespace bus::http;
using namespace bus::ws;
using bus::test::GetFreePort;
using bus::test::LoopThread;
using bus::test::RunInLoopSync;
using bus::ssl::Role;
using bus::ssl::SslContext;
using bus::ssl::SslSession;

namespace {

std::string CertDir() {
    return CODEBUS_TEST_CERT_DIR;
}

/// @brief 等待谓词成立
template <typename Fn>
bool WaitUntil(Fn&& predicate, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

/// @brief 客户端侧服务端帧头解析（服务端帧永不掩码——ParseFrameHeader 的服务端
///        规则不适用；简化：只解 opcode/fin/长度）
struct ClientFrameHeader {
    bool fin = false;
    Opcode opcode = Opcode::kContinuation;
    uint64_t payloadLen = 0;
};
/// @return 头部总长（含扩展长度）；数据不足返回 0
size_t ParseServerFrameHeader(const std::string& data, ClientFrameHeader* hdr) {
    if (data.size() < 2) {
        return 0;
    }
    const uint8_t b0 = static_cast<uint8_t>(data[0]);
    const uint8_t b1 = static_cast<uint8_t>(data[1]);
    hdr->fin = (b0 & 0x80) != 0;
    hdr->opcode = static_cast<Opcode>(b0 & 0x0F);
    const uint64_t len7 = b1 & 0x7F;  // 服务端→客户端帧 mask=0
    if (len7 <= 125) {
        hdr->payloadLen = len7;
        return 2;
    }
    if (len7 == 126) {
        if (data.size() < 4) {
            return 0;
        }
        hdr->payloadLen = (static_cast<uint64_t>(static_cast<uint8_t>(data[2])) << 8) |
                          static_cast<uint8_t>(data[3]);
        return 4;
    }
    if (data.size() < 10) {
        return 0;
    }
    uint64_t n = 0;
    for (int i = 2; i <= 9; ++i) {
        n = (n << 8) | static_cast<uint8_t>(data[i]);
    }
    hdr->payloadLen = n;
    return 10;
}

/// @brief 掩码客户端帧构造（文本/二进制/控制帧通用形态）
std::string MakeMaskedFrame(bool fin, Opcode op, std::string_view payload) {
    std::string wire;
    wire.push_back(static_cast<char>(((fin ? 0x80 : 0) | static_cast<uint8_t>(op)) & 0xFF));
    const uint64_t n = payload.size();
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
    const uint8_t key[4] = {0x5A, 0xC3, 0x11, 0x7E};
    wire.append(reinterpret_cast<const char*>(key), 4);
    std::string masked(payload);
    ApplyMask(masked.data(), masked.size(), key);
    wire.append(masked);
    return wire;
}

/// @brief TLS 上的 WS 测试客户端（TcpClient + decorator + 会话；101/帧队列互斥）
class WsOverTlsClient {
 public:
    WsOverTlsClient(EventLoop* loop, const std::string& addr) : m_loop(loop), m_ctx(SslContext::createClient()) {
        m_ctx->useCaFile(CertDir() + "/ca.crt");
        m_client = std::make_unique<TcpClient>(loop, addr);
        m_client->setNewConnectionDecorator([this](const TCPConnection::ptr& conn) {
            auto session = std::make_shared<SslSession>(Role::kClient, *m_ctx);
            session->setPeerName("127.0.0.1");
            conn->attachSsl(std::move(session));
        });
        m_client->setConnectionCallback([this](const TCPConnection::ptr& c) {
            if (c->status() == TCPConnection::Disconnected) {
                m_down.store(true);
            }
        });
        m_client->setMessageCallback([this](const TCPConnection::ptr&, Buffer* buf) {
            Slice readable = buf->PeekAll();
            if (readable.size() > 0) {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_plain.append(reinterpret_cast<const char*>(readable.data()), readable.size());
                ProcessLocked();
            }
            buf->SkipRead(readable.size());
        });
        m_client->connect();
    }

    /// @brief 发送升级握手（须在 TLS 数据期后调用）
    bool upgrade(const char* key) {
        std::string req = "GET /chat HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
        req += std::string("Sec-WebSocket-Key: ") + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        return sendRaw(req);
    }

    bool sendRaw(std::string_view data) {
        if (auto c = connection()) {
            c->send(data);
            return true;
        }
        return false;
    }
    bool sendMaskedFrame(bool fin, Opcode op, std::string_view payload) {
        return sendRaw(MakeMaskedFrame(fin, op, payload));
    }

    TCPConnection::ptr connection() const {
        return m_client->connection();
    }
    bool down() const {
        return m_down.load();
    }

    /// @brief 收尾（TcpClient 析构契约：连接断开 + stop）
    void Shutdown() {
        RunInLoopSync(m_loop, [this]() {
            if (auto c = connection()) {
                c->close();
            }
        });
        WaitUntil([this] { return m_down.load(); });
        m_client->stop();
    }
    /// @brief 升级握手是否完成（101 已收）
    bool upgraded() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_upgraded;
    }
    /// @brief 升级响应原文（断言 Accept 用）
    std::string upgradeResponse() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_upgrade_response;
    }
    /// @brief 取一帧（互斥队列；超时返回 nullopt）
    std::optional<std::pair<Opcode, std::string>> waitFrame(int timeoutMs = 5000) {
        if (!WaitUntil([this] {
                std::lock_guard<std::mutex> lk(m_mutex);
                return !m_frames.empty();
            },
                       timeoutMs)) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lk(m_mutex);
        auto f = std::move(m_frames.front());
        m_frames.erase(m_frames.begin());
        return f;
    }

 private:
    /// @brief 明文处理（m_mutex 已持）：101 检测 → 帧循环
    void ProcessLocked() {
        if (!m_upgraded) {
            const size_t hdr_end = m_plain.find("\r\n\r\n");
            if (hdr_end == std::string::npos) {
                return;
            }
            m_upgrade_response = m_plain.substr(0, hdr_end + 4);
            m_plain.erase(0, hdr_end + 4);
            m_upgraded = m_upgrade_response.starts_with("HTTP/1.1 101");
            return;  // 剩余字节（早期帧）下一批处理
        }
        for (;;) {
            ClientFrameHeader hdr;
            const size_t head = ParseServerFrameHeader(m_plain, &hdr);
            if (head == 0 || m_plain.size() < head + hdr.payloadLen) {
                return;  // 半帧：等更多字节
            }
            std::string payload = m_plain.substr(head, hdr.payloadLen);
            m_plain.erase(0, head + hdr.payloadLen);
            m_frames.emplace_back(hdr.opcode, std::move(payload));
        }
    }

    EventLoop* m_loop;
    std::unique_ptr<TcpClient> m_client;
    ssl::SslContext::ptr m_ctx;
    mutable std::mutex m_mutex;
    std::string m_plain;
    std::string m_upgrade_response;
    bool m_upgraded = false;
    std::vector<std::pair<Opcode, std::string>> m_frames;
    std::atomic<bool> m_down{false};
};

}  // namespace

class WssServerTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_port = GetFreePort();
        m_ws = std::make_shared<WsServer>(m_loop.loop());
        HttpServerConfig cfg;
        cfg.tls.enabled = true;
        cfg.tls.certFile = CertDir() + "/server.crt";
        cfg.tls.keyFile = CertDir() + "/server.key";
        m_server = std::make_unique<HttpServer>(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0, cfg);
        m_server->setUpgradeHandler(
            [this](const TCPConnection::ptr& conn, HttpRequest req, std::string_view leftover) {
                m_ws->handleUpgrade(conn, std::move(req), leftover);
            });
    }
    void TearDown() override {
        if (m_server) {
            std::atomic<bool> done{false};
            m_server->Stop([&] { done = true; });
            EXPECT_TRUE(WaitUntil([&] { return done.load(); }, 10000));
        }
        if (m_ws) {
            m_ws->Stop();
        }
        RunInLoopSync(m_loop.loop(), [] {});
    }
    void startAll() {
        m_ws->Start();
        m_server->Start();
        RunInLoopSync(m_loop.loop(), [] {});  // 启动屏障
    }
    /// @brief 客户端就绪屏障：TLS 数据期
    bool waitTlsEstablished(WsOverTlsClient& c) {
        return WaitUntil([&] {
            auto conn = c.connection();
            return conn != nullptr && conn->isSslEstablished();
        });
    }

    LoopThread m_loop;
    uint16_t m_port = 0;
    std::unique_ptr<HttpServer> m_server;
    std::shared_ptr<WsServer> m_ws;
};

/// [正] TLS 上 WS 升级握手：101 + Sec-WebSocket-Accept 计算值正确
TEST_F(WssServerTest, UpgradeHandshakeOverTls) {
    startAll();
    WsOverTlsClient c(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port));
    ASSERT_TRUE(waitTlsEstablished(c));
    ASSERT_TRUE(c.upgrade("dGhlIHNhbXBsZSBub25jZQ=="));

    ASSERT_TRUE(WaitUntil([&] { return c.upgraded(); })) << "no 101 over TLS";
    const std::string resp = c.upgradeResponse();
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 101"));
    const std::string accept = ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
    EXPECT_NE(resp.find(accept), std::string::npos) << "Sec-WebSocket-Acpect mismatch";

    c.Shutdown();
}

/// [正] wss echo：掩码文本帧 → 服务端 onMessage → sendText 回发 → 字节等
TEST_F(WssServerTest, EchoOverWss) {
    // echo 惯用法：连接观察者收集连接（WsServer::sendText 需连接引用），
    // onMessage 里回发（ioLoop 线程——send 线程安全）
    std::mutex targets_mutex;
    std::vector<TCPConnection::ptr> targets;
    m_ws->onMessage([&](const WsMessage& msg) {
        TCPConnection::ptr conn;
        {
            std::lock_guard<std::mutex> lk(targets_mutex);
            if (targets.empty()) {
                return;
            }
            conn = targets.front();
        }
        m_ws->sendText(conn, msg.payload);
    });
    m_server->setConnectionObserver([&](const TCPConnection::ptr& conn, bool up) {
        if (up) {
            std::lock_guard<std::mutex> lk(targets_mutex);
            targets.push_back(conn);
        }
    });
    startAll();

    WsOverTlsClient c(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port));
    ASSERT_TRUE(waitTlsEstablished(c));
    ASSERT_TRUE(c.upgrade("dGhlIHNhbXBsZSBub25jZQ=="));
    ASSERT_TRUE(WaitUntil([&] { return c.upgraded(); }));

    const std::string payload(8_KB, 'x');
    ASSERT_TRUE(c.sendMaskedFrame(true, Opcode::kText, payload));
    auto frame = c.waitFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->first, Opcode::kText);
    EXPECT_EQ(frame->second.size(), payload.size());
    EXPECT_EQ(std::memcmp(frame->second.data(), payload.data(), payload.size()), 0);

    c.Shutdown();
}

/// [正] wss 关闭握手：掩码 close → 服务端 echo close → 连接收尾
TEST_F(WssServerTest, CloseHandshakeOverWss) {
    startAll();
    WsOverTlsClient c(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port));
    ASSERT_TRUE(waitTlsEstablished(c));
    ASSERT_TRUE(c.upgrade("dGhlIHNhbXBsZSBub25jZQ=="));
    ASSERT_TRUE(WaitUntil([&] { return c.upgraded(); }));

    std::string close_payload;
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(static_cast<char>(0xE8));  // 1000
    ASSERT_TRUE(c.sendMaskedFrame(true, Opcode::kClose, close_payload));

    auto frame = c.waitFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->first, Opcode::kClose);  // echo close
    EXPECT_TRUE(WaitUntil([&] { return c.down(); }, 5000)) << "connection not finalized after close over TLS";

    c.Shutdown();  // 已断开：close 幂等 + stop 置停
}

/// [正] U6 心跳与 TLS 叠加：短配置 idle=1 → TLS 上收到服务端 ping → 回 pong 存活
TEST_F(WssServerTest, ServerIdlePingOverWss) {
    WsServerConfig cfg;
    cfg.idlePingSec = 1;
    cfg.pongTimeoutSec = 1;
    cfg.closeReplySec = 1;
    m_ws = std::make_shared<WsServer>(m_loop.loop(), cfg);
    m_server->setUpgradeHandler(
        [this](const TCPConnection::ptr& conn, HttpRequest req, std::string_view leftover) {
            m_ws->handleUpgrade(conn, std::move(req), leftover);
        });
    startAll();

    WsOverTlsClient c(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port));
    ASSERT_TRUE(waitTlsEstablished(c));
    ASSERT_TRUE(c.upgrade("dGhlIHNhbXBsZSBub25jZQ=="));
    ASSERT_TRUE(WaitUntil([&] { return c.upgraded(); }));

    auto ping = c.waitFrame(5000);
    ASSERT_TRUE(ping.has_value()) << "no idle ping over TLS";
    EXPECT_EQ(ping->first, Opcode::kPing);
    ASSERT_TRUE(c.sendMaskedFrame(true, Opcode::kPong, ping->second));

    // pong 清死线：越过 pongTimeout 后再收下一轮 ping（连接仍活）
    auto ping2 = c.waitFrame(5000);
    ASSERT_TRUE(ping2.has_value()) << "connection dead after pong (deadline not cleared over TLS)";
    EXPECT_EQ(ping2->first, Opcode::kPing);
    ASSERT_TRUE(c.sendMaskedFrame(true, Opcode::kPong, ping2->second));

    c.Shutdown();
}

/// [正] wss 分片重组：256KB 三片发送 → 服务端重组 → 单帧 echo 字节等
TEST_F(WssServerTest, FragmentedReassemblyOverWss) {
    std::mutex targets_mutex;
    std::vector<TCPConnection::ptr> targets;
    m_ws->onMessage([&](const WsMessage& msg) {
        TCPConnection::ptr conn;
        {
            std::lock_guard<std::mutex> lk(targets_mutex);
            if (targets.empty()) {
                return;
            }
            conn = targets.front();
        }
        m_ws->sendText(conn, msg.payload);
    });
    m_server->setConnectionObserver([&](const TCPConnection::ptr& conn, bool up) {
        if (up) {
            std::lock_guard<std::mutex> lk(targets_mutex);
            targets.push_back(conn);
        }
    });
    startAll();

    WsOverTlsClient c(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port));
    ASSERT_TRUE(waitTlsEstablished(c));
    ASSERT_TRUE(c.upgrade("dGhlIHNhbXBsZSBub25jZQ=="));
    ASSERT_TRUE(WaitUntil([&] { return c.upgraded(); }));

    // 合法 UTF-8 载荷（validateText=true 默认开——任意字节流会被 1007 正确拒绝）
    std::string payload(256_KB, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('a' + (i % 26));
    }
    // 分片：text(fin=0) + cont(fin=0) + cont(fin=1)
    const std::string_view whole(payload);
    const size_t s1 = payload.size() / 3;
    const size_t s2 = payload.size() * 2 / 3;
    ASSERT_TRUE(c.sendMaskedFrame(false, Opcode::kText, whole.substr(0, s1)));
    ASSERT_TRUE(c.sendMaskedFrame(false, Opcode::kContinuation, whole.substr(s1, s2 - s1)));
    ASSERT_TRUE(c.sendMaskedFrame(true, Opcode::kContinuation, whole.substr(s2)));

    auto frame = c.waitFrame(10000);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->first, Opcode::kText);
    ASSERT_EQ(frame->second.size(), payload.size());
    EXPECT_EQ(std::memcmp(frame->second.data(), payload.data(), payload.size()), 0);

    c.Shutdown();
}
