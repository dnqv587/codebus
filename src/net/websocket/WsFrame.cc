//***********************************************
// name      : WsFrame.cc
// brief     : WebSocket 帧编解码实现
// attention : None
//***********************************************
#include "net/websocket/WsFrame.h"

namespace bus::ws {

ParseStatus ParseFrameHeader(const char* data, size_t len, FrameHeader* hdr, size_t* consumed,
                             uint16_t* close_code) {
    *close_code = 0;
    if (len < 2) {
        return ParseStatus::kNeedMore;
    }
    const uint8_t b0 = static_cast<uint8_t>(data[0]);
    const uint8_t b1 = static_cast<uint8_t>(data[1]);

    hdr->fin = (b0 & 0x80) != 0;
    const uint8_t rsv = (b0 >> 4) & 0x07;
    hdr->opcode = static_cast<Opcode>(b0 & 0x0F);
    hdr->masked = (b1 & 0x80) != 0;

    // 服务端规则（RFC 6455 §5.2 全表）
    if (rsv != 0) {
        *close_code = 1002;  // RSV 非零（无扩展协商）
        return ParseStatus::kProtocolError;
    }
    const auto op = static_cast<uint8_t>(hdr->opcode);
    const bool is_control = (op & 0x08) != 0;
    if (op != 0x0 && op != 0x1 && op != 0x2 && op != 0x8 && op != 0x9 && op != 0xA) {
        *close_code = 1002;  // 未知 opcode
        return ParseStatus::kProtocolError;
    }
    if (is_control && !hdr->fin) {
        *close_code = 1002;  // 控制帧不可分片
        return ParseStatus::kProtocolError;
    }
    if (!hdr->masked) {
        *close_code = 1002;  // 客户端→服务端帧必须掩码（§5.1）
        return ParseStatus::kProtocolError;
    }

    // 长度三档
    uint8_t len7 = b1 & 0x7F;
    size_t ext = 0;
    if (len7 == 126) {
        ext = 2;
    } else if (len7 == 127) {
        ext = 8;
    }
    const size_t mask_off = 2 + ext;
    const size_t frame_head = mask_off + 4;  // 掩码键 4 字节（masked 恒真走到此处）
    if (len < frame_head) {
        return ParseStatus::kNeedMore;
    }
    uint64_t plen = len7;
    if (ext == 2) {
        plen = (static_cast<uint8_t>(data[2]) << 8) | static_cast<uint8_t>(data[3]);
    } else if (ext == 8) {
        plen = 0;
        for (int i = 0; i < 8; ++i) {
            plen = (plen << 8) | static_cast<uint8_t>(data[2 + i]);
        }
        if (plen & 0x8000000000000000ull) {
            *close_code = 1002;  // 最高位必须为 0（§5.2）
            return ParseStatus::kProtocolError;
        }
    }
    if (is_control && plen > 125) {
        *close_code = 1002;  // 控制帧负载 ≤125
        return ParseStatus::kProtocolError;
    }
    hdr->payloadLen = plen;
    for (int i = 0; i < 4; ++i) {
        hdr->maskKey[i] = static_cast<uint8_t>(data[mask_off + i]);
    }
    *consumed = frame_head;
    return ParseStatus::kOk;
}

void ApplyMask(char* data, size_t len, const uint8_t key[4]) noexcept {
    for (size_t i = 0; i < len; ++i) {
        data[i] = static_cast<char>(static_cast<uint8_t>(data[i]) ^ key[i & 3]);
    }
}

void AppendFrame(std::string& out, Opcode op, std::string_view payload) {
    out.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(op)));  // FIN=1，无掩码（服务端）
    const uint64_t n = payload.size();
    if (n <= 125) {
        out.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
        }
    }
    out.append(payload);
}

}  // namespace bus::ws
