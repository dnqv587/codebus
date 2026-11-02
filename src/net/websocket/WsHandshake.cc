//***********************************************
// name      : WsHandshake.cc
// brief     : WebSocket 握手实现（SHA1/Base64 自实现）
// attention : None
//***********************************************
#include "net/websocket/WsHandshake.h"

#include <array>
#include <cstring>
#include <cstdint>

#include "net/http/HttpUtil.h"

namespace bus::ws {

namespace {

/// @brief RFC 6455 握手 GUID
constexpr std::string_view kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// @brief SHA1 上下文（FIPS 180-4 标准实现）
struct Sha1Ctx {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t total = 0;
    uint8_t buf[64] = {};
    size_t buflen = 0;

    void process(const uint8_t* block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        total += len;
        while (len > 0) {
            const size_t take = std::min(len, sizeof(buf) - buflen);
            std::memcpy(buf + buflen, data, take);
            buflen += take;
            data += take;
            len -= take;
            if (buflen == 64) {
                process(buf);
                buflen = 0;
            }
        }
    }

    /// @brief 终结并输出 20 字节摘要
    std::array<uint8_t, 20> finish() {
        const uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (buflen != 56) {
            update(&pad, 1);
        }
        uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) {
            lenb[i] = static_cast<uint8_t>(bits >> (8 * (7 - i)));
        }
        update(lenb, 8);
        std::array<uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
        }
        return out;
    }
};

/// @brief Base64 编码（标准字母表，含 padding）
std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (i + 1 < len ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                           (i + 2 < len ? static_cast<uint32_t>(data[i + 2]) : 0);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kAlphabet[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kAlphabet[v & 0x3F] : '=');
    }
    return out;
}

}  // namespace

std::string Sha1Hex(std::string_view input) {
    Sha1Ctx ctx;
    ctx.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    const auto digest = ctx.finish();
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (uint8_t b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

std::string ComputeAcceptKey(std::string_view key) {
    const std::string concat = std::string(key) + std::string(kWsGuid);
    Sha1Ctx ctx;
    ctx.update(reinterpret_cast<const uint8_t*>(concat.data()), concat.size());
    const auto digest = ctx.finish();
    return Base64Encode(digest.data(), digest.size());
}

bool ValidateUpgradeRequest(const bus::http::HttpRequest& req, std::string* reason) {
    auto fail = [reason](const char* why) {
        if (reason != nullptr) {
            *reason = why;
        }
        return false;
    };
    auto ver = req.getHeader("sec-websocket-version");
    if (!ver.has_value() || *ver != "13") {
        return fail("unsupported Sec-WebSocket-Version (want 13)");
    }
    auto key = req.getHeader("sec-websocket-key");
    if (!key.has_value()) {
        return fail("missing Sec-WebSocket-Key");
    }
    // key = 16 字节随机值的 base64：定长 24 字符且以 "==» 结尾
    if (key->size() != 24 || !key->ends_with("==")) {
        return fail("malformed Sec-WebSocket-Key");
    }
    auto conn = req.getHeader("connection");
    if (!conn.has_value() || conn->find("pgrade") == std::string_view::npos) {  // "Upgrade"（大小写不敏感片段）
        return fail("Connection header must include Upgrade");
    }
    return true;
}

std::string Build101Response(std::string_view accept) {
    std::string out = "HTTP/1.1 101 Switching Protocols\r\n";
    out += "Upgrade: websocket\r\n";
    out += "Connection: Upgrade\r\n";
    out += "Sec-WebSocket-Accept: ";
    out += accept;
    out += "\r\n\r\n";
    return out;
}

}  // namespace bus::ws
