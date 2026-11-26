//***********************************************
// name      : WsClientTest.cc
// brief     : WebSocket 客户端端到端测试（对自身 HttpServer+WsServer 桥接服务端）
// attention : 客户端独立 loop；TearDown 经 stop + 连接清空 + FIFO 屏障后析构
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/http/HttpServer.h"
#include "net/websocket/WsClient.h"
#include "net/websocket/WsHandshake.h"
#include "net/websocket/WsServer.h"

using namespace bus;
using namespace bus::http;
using namespace bus::ws;
using namespace bus::test;

class WsClientTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_port = GetFreePort();
        m_ws = std::make_shared<WsServer>(m_loop.loop());
        m_server = std::make_unique<HttpServer>(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0);
        m_server->setUpgradeHandler([this](const TCPConnection::ptr& conn, HttpRequest req,
                                           std::string_view leftover) {
            m_conn = conn;  // weak：不延长服务端连接生命周期（fd RAII 最末引用关闭）
            m_ws->handleUpgrade(conn, std::move(req), leftover);
        });
        m_ws->onMessage([this](const WsMessage& msg) {
            if (auto c = m_conn.lock()) {
                if (msg.isText) {
                    m_ws->sendText(c, msg.payload);
                } else {
                    m_ws->sendBinary(c, msg.payload);
                }
            }
        });
    }
    void TearDown() override {
        if (m_client) {
            m_client->stop();
            WaitFor([&] { return m_client->connection() == nullptr; }, std::chrono::milliseconds(10000));
            RunInLoopSync(m_client_loop.loop(), [] {});
            m_client.reset();
        }
        if (m_server) {
            std::atomic<bool> done{false};
            m_server->Stop([&] { done = true; });
            ASSERT_TRUE(WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000)));
        }
        if (m_ws) {
            m_ws->Stop();
        }
        RunInLoopSync(m_loop.loop(), [] {});
    }
    void startAndWait() {
        m_ws->Start();
        m_server->Start();
        ASSERT_TRUE(WaitFor(
            [&] {
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) return false;
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
    void makeClient(WsClientConfig cfg = {}) {
        m_client = std::make_unique<WsClient>(m_client_loop.loop(),
                                              "127.0.0.1:" + std::to_string(m_port), cfg);
    }

    LoopThread m_loop;
    LoopThread m_client_loop;
    uint16_t m_port = 0;
    std::shared_ptr<WsServer> m_ws;
    std::unique_ptr<HttpServer> m_server;
    std::weak_ptr<TCPConnection> m_conn;
    std::unique_ptr<WsClient> m_client;
};

/// [正] 握手成功 → onOpen（H5.2 W2）
TEST_F(WsClientTest, HandshakeOpens) {
    startAndWait();
    makeClient();
    std::atomic<bool> opened{false};
    m_client->setOpenCallback([&] { opened = true; });
    m_client->connect("/chat");
    EXPECT_TRUE(WaitFor([&] { return opened.load(); }, std::chrono::milliseconds(5000)));
}
/// [正] 文本回显（H5.2 W3）：sendText 掩码发出 → 服务端解回 → onMessage 文本
TEST_F(WsClientTest, TextEcho) {
    startAndWait();
    makeClient();
    std::atomic<bool> opened{false};
    std::mutex mu;
    std::vector<WsMessage> msgs;
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setMessageCallback([&](const WsMessage& m) {
        std::lock_guard<std::mutex> lk(mu);
        msgs.push_back(m);
    });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return opened.load(); }, std::chrono::milliseconds(5000)));
    m_client->sendText("hello ws");
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lk(mu);
        return msgs.size() == 1;
    }, std::chrono::milliseconds(5000)));
    std::lock_guard<std::mutex> lk(mu);
    EXPECT_TRUE(msgs[0].isText);
    EXPECT_EQ(msgs[0].payload, "hello ws");
}

/// [正] 二进制保真（H5.2 W4）：含 NUL 字节往返一致
TEST_F(WsClientTest, BinaryEchoFidelity) {
    startAndWait();
    makeClient();
    std::atomic<bool> opened{false};
    std::mutex mu;
    std::vector<WsMessage> msgs;
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setMessageCallback([&](const WsMessage& m) {
        std::lock_guard<std::mutex> lk(mu);
        msgs.push_back(m);
    });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return opened.load(); }, std::chrono::milliseconds(5000)));
    const std::string bin("\x00\x01\xff\xfe", 4);
    m_client->sendBinary(bin);
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lk(mu);
        return msgs.size() == 1;
    }, std::chrono::milliseconds(5000)));
    std::lock_guard<std::mutex> lk(mu);
    EXPECT_FALSE(msgs[0].isText);
    EXPECT_EQ(msgs[0].payload, bin);
}

/// [正] ping → pong 可观测（H5.2 W5）
TEST_F(WsClientTest, PingPong) {
    startAndWait();
    makeClient();
    std::atomic<bool> opened{false};
    std::atomic<int> pongs{0};
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setPongCallback([&] { ++pongs; });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return opened.load(); }, std::chrono::milliseconds(5000)));
    m_client->ping();
    EXPECT_TRUE(WaitFor([&] { return pongs.load() >= 1; }, std::chrono::milliseconds(5000)));
}

/// [正] 客户端主动关闭握手（H5.2 W6a）：close(1000, "bye") → onClose 收口 + TCP 关
TEST_F(WsClientTest, ClientInitiatedClose) {
    startAndWait();
    makeClient();
    std::atomic<bool> opened{false};
    std::atomic<bool> closed{false};
    std::atomic<uint16_t> code{0};
    std::string reason;
    std::mutex mu;
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setCloseCallback([&](uint16_t c, std::string_view r) {
        std::lock_guard<std::mutex> lk(mu);
        code = c;
        reason = std::string(r);
        closed = true;
    });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return opened.load(); }, std::chrono::milliseconds(5000)));
    m_client->close(1000, "bye");
    ASSERT_TRUE(WaitFor([&] { return closed.load(); }, std::chrono::milliseconds(5000)));
    EXPECT_EQ(code.load(), 1000u);
    std::lock_guard<std::mutex> lk(mu);
    EXPECT_TRUE(reason.empty());  // 服务端 echo 仅携 code（RFC §5.5.1 允许省略 reason）
    EXPECT_TRUE(WaitFor([&] { return m_client->connection() == nullptr; }, std::chrono::milliseconds(5000)));
}

namespace {

/// @brief 裸 WS 对端（握手响应可配 + 任意帧注入 + close echo；违规/分片/服务端关闭用例）
class RawWsPeer {
 public:
    RawWsPeer() {
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        ::listen(m_listen, 1);
    }
    ~RawWsPeer() {
        stop();
        if (m_listen >= 0) ::close(m_listen);
    }
    uint16_t port() const { return m_port; }
    /// @brief 启动对端线程：读升级请求 → 发 response（{KEY} 替换为请求 key 的 Accept）→ 发 followup → close echo 循环
    void start(std::string response, std::string followup) {
        m_thread = std::thread([this, response, followup] {
            int fd = ::accept(m_listen, nullptr, nullptr);
            if (fd < 0) return;
            m_conn.store(fd);
            std::string req;
            char tmp[2048];
            while (req.find("\r\n\r\n") == std::string::npos) {
                ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
                if (n <= 0) { ::close(fd); m_conn.store(-1); return; }
                req.append(tmp, static_cast<size_t>(n));
            }
            std::string resp = response;
            const std::string kmark = "Sec-WebSocket-Key: ";
            size_t kp = req.find(kmark);
            if (kp != std::string::npos) {
                size_t ke = req.find("\r\n", kp);
                std::string key = req.substr(kp + kmark.size(), ke - kp - kmark.size());
                const std::string ph = "{ACCEPT}";
                size_t pp = resp.find(ph);
                if (pp != std::string::npos) resp.replace(pp, ph.size(), ComputeAcceptKey(key));
            }
            ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
            if (!followup.empty()) ::send(fd, followup.data(), followup.size(), MSG_NOSIGNAL);
            // 收客户端帧：close → 解掩码取 code 回 echo（ unmasked）；其余丢弃
            for (;;) {
                ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
                if (n <= 0) break;
                if (n >= 2 && (static_cast<uint8_t>(tmp[0]) & 0x0F) == 0x8) {
                    FrameHeader hdr;
                    size_t consumed = 0;
                    uint16_t cc = 0;
                    if (ParseFrameHeader(tmp, static_cast<size_t>(n), &hdr, &consumed, &cc) == ParseStatus::kOk &&
                        static_cast<size_t>(n) >= consumed + hdr.payloadLen) {
                        std::string payload(tmp + consumed, hdr.payloadLen);
                        ApplyMask(payload.data(), payload.size(), hdr.maskKey);
                        if (payload.size() >= 2) {
                            m_close_code.store(static_cast<uint16_t>(static_cast<uint8_t>(payload[0]) << 8 |
                                                                    static_cast<uint8_t>(payload[1])));
                        }
                        std::string echo;
                        AppendFrame(echo, Opcode::kClose, payload);
                        ssize_t sn = ::send(fd, echo.data(), echo.size(), MSG_NOSIGNAL);
                    }
                }
            }
            ::close(fd);
            m_conn.store(-1);
        });
    }
    void stop() {
        int fd = m_conn.exchange(-1);
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        if (m_thread.joinable()) m_thread.join();
    }
    std::atomic<uint16_t> m_close_code{0};

 private:
    int m_listen = -1;
    uint16_t m_port = 0;
    std::atomic<int> m_conn{-1};
    std::thread m_thread;
};

/// @brief 手工构造掩码文本帧（服务端发掩码帧 = 协议违规注入）
std::string MaskedTextFrame(std::string_view payload) {
    std::string out;
    out.push_back(static_cast<char>(0x81));
    out.push_back(static_cast<char>(0x80 | payload.size()));
    const uint8_t key[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    out.append(reinterpret_cast<const char*>(key), 4);
    const size_t off = out.size();
    out.append(payload);
    ApplyMask(out.data() + off, payload.size(), key);
    return out;
}

}  // namespace

/// [正] 服务端主动 close（H5.2 W6b）：onClose 携对端 code/reason + TCP 关
TEST_F(WsClientTest, ServerInitiatedClose) {
    RawWsPeer peer;
    std::string close_frame;
    AppendFrame(close_frame, Opcode::kClose, std::string("\x03\xe8" "srv", 5));  // 1000 + "srv"
    peer.start("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: {ACCEPT}\r\n\r\n", close_frame);
    m_client = std::make_unique<WsClient>(m_client_loop.loop(), "127.0.0.1:" + std::to_string(peer.port()));
    std::atomic<bool> opened{false};
    std::atomic<bool> closed{false};
    std::atomic<uint16_t> code{0};
    std::mutex mu;
    std::string reason;
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setCloseCallback([&](uint16_t c, std::string_view r) {
        std::lock_guard<std::mutex> lk(mu);
        code = c;
        reason = std::string(r);
        closed = true;
    });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return opened.load(); }, std::chrono::milliseconds(5000)));
    ASSERT_TRUE(WaitFor([&] { return closed.load(); }, std::chrono::milliseconds(5000)));
    EXPECT_EQ(code.load(), 1000u);
    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_EQ(reason, "srv");
    }
    EXPECT_TRUE(WaitFor([&] { return m_client->connection() == nullptr; }, std::chrono::milliseconds(5000)));
    peer.stop();
}

/// [界] 服务端掩码帧 = 协议违规（H5.2 W7）：客户端回 close 1002
TEST_F(WsClientTest, MaskedServerFrameRejected1002) {
    RawWsPeer peer;
    peer.start("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: {ACCEPT}\r\n\r\n", MaskedTextFrame("bad"));
    m_client = std::make_unique<WsClient>(m_client_loop.loop(), "127.0.0.1:" + std::to_string(peer.port()));
    std::atomic<bool> opened{false};
    std::atomic<bool> closed{false};
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setCloseCallback([&](uint16_t, std::string_view) { closed = true; });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return opened.load(); }, std::chrono::milliseconds(5000)));
    ASSERT_TRUE(WaitFor([&] { return closed.load(); }, std::chrono::milliseconds(5000)));
    EXPECT_EQ(peer.m_close_code.load(), 1002u);  // 客户端发出的 close 码
    peer.stop();
}

/// [正] 分片重组（H5.2 W8）：text(fin=0) + continuation(fin=1) → 单消息
TEST_F(WsClientTest, FragmentedMessageReassembled) {
    RawWsPeer peer;
    std::string frag;
    frag.push_back(static_cast<char>(0x01));  // fin=0 text
    frag.push_back(static_cast<char>(3));
    frag.append("hel");
    frag.push_back(static_cast<char>(0x80));  // fin=1 continuation
    frag.push_back(static_cast<char>(2));
    frag.append("lo");
    peer.start("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: {ACCEPT}\r\n\r\n", frag);
    m_client = std::make_unique<WsClient>(m_client_loop.loop(), "127.0.0.1:" + std::to_string(peer.port()));
    std::atomic<bool> opened{false};
    std::mutex mu;
    std::vector<WsMessage> msgs;
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setMessageCallback([&](const WsMessage& m) {
        std::lock_guard<std::mutex> lk(mu);
        msgs.push_back(m);
    });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return opened.load(); }, std::chrono::milliseconds(5000)));
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lk(mu);
        return msgs.size() == 1;
    }, std::chrono::milliseconds(5000)));
    std::lock_guard<std::mutex> lk(mu);
    EXPECT_TRUE(msgs[0].isText);
    EXPECT_EQ(msgs[0].payload, "hello");
    peer.stop();
}

/// [界] Accept 失配（H5.2 W9）：error cb + 连接关，不进假就绪
TEST_F(WsClientTest, HandshakeAcceptMismatchFails) {
    RawWsPeer peer;
    peer.start("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n", "");
    m_client = std::make_unique<WsClient>(m_client_loop.loop(), "127.0.0.1:" + std::to_string(peer.port()));
    std::atomic<bool> opened{false};
    std::atomic<bool> errored{false};
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setErrorCallback([&](std::string_view) { errored = true; });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return errored.load(); }, std::chrono::milliseconds(5000)));
    EXPECT_FALSE(opened.load());
    EXPECT_TRUE(WaitFor([&] { return m_client->connection() == nullptr; }, std::chrono::milliseconds(5000)));
    peer.stop();
}

/// [界] 非 101 完整响应（H5.2 故事 24 补覆盖）：error cb，不进假就绪
TEST_F(WsClientTest, HandshakeNon101Fails) {
    RawWsPeer peer;
    peer.start("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", "");
    m_client = std::make_unique<WsClient>(m_client_loop.loop(), "127.0.0.1:" + std::to_string(peer.port()));
    std::atomic<bool> opened{false};
    std::atomic<bool> errored{false};
    m_client->setOpenCallback([&] { opened = true; });
    m_client->setErrorCallback([&](std::string_view) { errored = true; });
    m_client->connect("/chat");
    ASSERT_TRUE(WaitFor([&] { return errored.load(); }, std::chrono::milliseconds(5000)));
    EXPECT_FALSE(opened.load());
    EXPECT_TRUE(WaitFor([&] { return m_client->connection() == nullptr; }, std::chrono::milliseconds(5000)));
    peer.stop();
}
