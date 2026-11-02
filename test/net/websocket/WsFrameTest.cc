//***********************************************
// name      : WsFrameTest.cc
// brief     : WebSocket 帧编解码纯函数测试（RFC 6455 全表/SHA1 向量/握手）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <string>

#include "net/websocket/WsFrame.h"
#include "net/websocket/WsHandshake.h"

using namespace bus;
using namespace bus::ws;

/// [正] SHA1 标准向量
TEST(WsFrameTest, Sha1Vectors) {
    EXPECT_EQ(Sha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(Sha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(Sha1Hex("The quick brown fox jumps over the lazy dog"),
              "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

/// [正] RFC 6455 §1.3 端到端握手样本
TEST(WsFrameTest, AcceptKeyRfcVector) {
    EXPECT_EQ(ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

/// [正] 握手请求校验
TEST(WsFrameTest, ValidateUpgradeRequest) {
    auto mk = [](const char* ver, const char* key) {
        http::HttpRequest req;
        req.setMethod(http::HttpMethod::kGET);
        req.setPath("/chat");
        if (ver) {
            req.addHeaderRaw("Sec-WebSocket-Version", ver);
        }
        if (key) {
            req.addHeaderRaw("Sec-WebSocket-Key", key);
        }
        req.addHeaderRaw("Upgrade", "websocket");
        req.addHeaderRaw("Connection", "Upgrade");
        return req;
    };
    std::string reason;
    EXPECT_TRUE(ValidateUpgradeRequest(mk("13", "dGhlIHNhbXBsZSBub25jZQ=="), &reason)) << reason;
    EXPECT_FALSE(ValidateUpgradeRequest(mk("12", "dGhlIHNhbXBsZSBub25jZQ=="), &reason));  // 版本
    EXPECT_FALSE(ValidateUpgradeRequest(mk("13", nullptr), &reason));                     // 缺 key
    EXPECT_FALSE(ValidateUpgradeRequest(mk("13", "short"), &reason));                     // 非法 key
}

/// @brief 构造客户端帧（掩码——服务端解析器的合法输入）
std::string MakeClientFrame(Opcode op, std::string_view payload, bool fin = true) {
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
    const uint8_t key[4] = {0x11, 0x22, 0x33, 0x44};
    wire.append(reinterpret_cast<const char*>(key), 4);
    std::string masked(payload);
    ApplyMask(masked.data(), masked.size(), key);
    wire.append(masked);
    return wire;
}

/// [正] 帧往返：客户端掩码帧（全 opcode/长度三档）
TEST(WsFrameTest, FrameRoundTrip) {
    for (auto [op, payload] : {std::pair<Opcode, std::string>{Opcode::kText, "hello"},
                               {Opcode::kBinary, std::string(200, 'x')},    // 16bit 长度档
                               {Opcode::kBinary, std::string(70000, 'y')},  // 64bit 长度档
                               {Opcode::kPing, "hb"},
                               {Opcode::kClose, ""}}) {
        const std::string wire = MakeClientFrame(op, payload);
        FrameHeader hdr;
        size_t consumed = 0;
        uint16_t cc = 0;
        ASSERT_EQ(ParseFrameHeader(wire.data(), wire.size(), &hdr, &consumed, &cc), ParseStatus::kOk)
            << " op=" << static_cast<int>(op);
        EXPECT_EQ(hdr.opcode, op);
        EXPECT_TRUE(hdr.fin);
        EXPECT_TRUE(hdr.masked);
        EXPECT_EQ(hdr.payloadLen, payload.size());
        std::string got = wire.substr(consumed, payload.size());
        ApplyMask(got.data(), got.size(), hdr.maskKey);
        EXPECT_EQ(got, payload);
    }
}

/// [正] AppendFrame（服务端帧）：掩码位恒零、长度三档编码
TEST(WsFrameTest, AppendFrameServerForm) {
    std::string wire;
    AppendFrame(wire, Opcode::kText, "hi");
    ASSERT_GE(wire.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(wire[1]) & 0x80, 0u);  // 服务端不掩码
    EXPECT_EQ(wire.substr(2), "hi");
    EXPECT_EQ(wire.size(), 4u);

    std::string big(300, 'z');
    std::string w2;
    AppendFrame(w2, Opcode::kBinary, big);
    EXPECT_EQ(static_cast<uint8_t>(w2[1]), 126);  // 16bit 档
    EXPECT_EQ(w2.substr(4), big);
}

/// [界] 协议错误全表（RFC 6455 §5.2 服务端规则）
TEST(WsFrameTest, ProtocolErrorMatrix) {
    auto expect_close = [](const std::string& wire, uint16_t want) {
        FrameHeader hdr;
        size_t consumed = 0;
        uint16_t cc = 0;
        ASSERT_EQ(ParseFrameHeader(wire.data(), wire.size(), &hdr, &consumed, &cc), ParseStatus::kProtocolError);
        EXPECT_EQ(cc, want);
    };
    // 客户端帧未掩码 → 1002
    std::string nomask;
    AppendFrame(nomask, Opcode::kText, "x");
    expect_close(nomask, 1002);
    // RSV≠0 → 1002
    std::string rsv(1, static_cast<char>(0x81 | 0x40));  // fin|rsv1|text
    rsv += '\x01';
    expect_close(rsv, 1002);
    // 控制帧分片（ping fin=0）→ 1002
    std::string fragping(1, static_cast<char>(0x09 & 0x7F));  // ping 无 fin
    fragping += '\x00';
    expect_close(fragping, 1002);
    // 控制帧 payload>125 → 1002（ping 126 字节）
    std::string bigping(1, static_cast<char>(0x89));
    bigping += static_cast<char>(0x80 | 126);  // masked len=126
    bigping += "\x01\x02\x03\x04" + std::string(126, 'p');
    expect_close(bigping, 1002);
    // 未知 opcode（0x3）→ 1002
    std::string badop(1, static_cast<char>(0x83));  // fin|0x3
    badop += '\x00';
    expect_close(badop, 1002);
}

/// [正] 掩码 XOR 幂等
TEST(WsFrameTest, MaskXor) {
    const uint8_t key[4] = {0x01, 0x02, 0x03, 0x04};
    std::string data = "mask me mask me";
    std::string orig = data;
    ApplyMask(data.data(), data.size(), key);
    EXPECT_NE(data, orig);
    ApplyMask(data.data(), data.size(), key);
    EXPECT_EQ(data, orig);
}

/// [正] 不足一字节 → NeedMore
TEST(WsFrameTest, NeedMore) {
    FrameHeader hdr;
    size_t consumed = 0;
    uint16_t cc = 0;
    EXPECT_EQ(ParseFrameHeader("\x81", 1, &hdr, &consumed, &cc), ParseStatus::kNeedMore);
}
