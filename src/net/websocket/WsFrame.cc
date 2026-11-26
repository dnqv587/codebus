//***********************************************
// name      : WsFrame.cc
// brief     : WebSocket 帧编解码实现
// attention : None
//***********************************************
#include "net/websocket/WsFrame.h"

#include <random>

namespace bus::ws {

namespace {

/// @brief 帧头解析公共实现（视角参数化：require_masked=true 服务端收帧，false 客户端收帧）
ParseStatus ParseFrameHeaderImpl(const char* data, size_t len, FrameHeader* hdr, size_t* consumed,
                                 uint16_t* close_code, bool require_masked) {
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

    // 协议规则全表（RFC 6455 §5.2）
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
    if (hdr->masked != require_masked) {
        *close_code = 1002;  // 掩码位视角规则（§5.1：客→服必掩码，服→客必不掩码）
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
    const size_t frame_head = mask_off + (hdr->masked ? 4 : 0);
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
    if (hdr->masked) {
        for (int i = 0; i < 4; ++i) {
            hdr->maskKey[i] = static_cast<uint8_t>(data[mask_off + i]);
        }
    }
    *consumed = frame_head;
    return ParseStatus::kOk;
}

/// @brief 帧掩码随机键（系统随机源逐帧抽取——RFC §5.3 不可预测性；thread_local 缓冲免分配）
const uint8_t* RandomMaskKey() {
    static thread_local std::random_device rd;
    static thread_local uint8_t key[4];
    for (int i = 0; i < 4; ++i) {
        key[i] = static_cast<uint8_t>(rd() & 0xFF);
    }
    return key;
}

/// @brief 帧追加公共实现（长度三档编码单点；掩码位参数化——服务端/客户端同法）
void AppendFrameImpl(std::string& out, Opcode op, std::string_view payload, bool masked) {
    out.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(op)));  // FIN=1
    const uint64_t n = payload.size();
    const uint8_t mask_bit = masked ? 0x80 : 0x00;
    if (n <= 125) {
        out.push_back(static_cast<char>(mask_bit | n));
    } else if (n <= 0xFFFF) {
        out.push_back(static_cast<char>(mask_bit | 126));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(static_cast<char>(mask_bit | 127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
        }
    }
    if (masked) {
        const uint8_t* key = RandomMaskKey();
        out.append(reinterpret_cast<const char*>(key), 4);
        const size_t payload_off = out.size();
        out.append(payload);
        ApplyMask(out.data() + payload_off, payload.size(), key);
    } else {
        out.append(payload);
    }
}

}  // namespace

ParseStatus ParseFrameHeader(const char* data, size_t len, FrameHeader* hdr, size_t* consumed,
                             uint16_t* close_code) {
    return ParseFrameHeaderImpl(data, len, hdr, consumed, close_code, /*require_masked=*/true);
}

ParseStatus ParseFrameHeaderClient(const char* data, size_t len, FrameHeader* hdr, size_t* consumed,
                                   uint16_t* close_code) {
    return ParseFrameHeaderImpl(data, len, hdr, consumed, close_code, /*require_masked=*/false);
}

void ApplyMask(char* data, size_t len, const uint8_t key[4]) noexcept {
    for (size_t i = 0; i < len; ++i) {
        data[i] = static_cast<char>(static_cast<uint8_t>(data[i]) ^ key[i & 3]);
    }
}

void AppendFrame(std::string& out, Opcode op, std::string_view payload) {
    AppendFrameImpl(out, op, payload, /*masked=*/false);
}

void AppendFrameMasked(std::string& out, Opcode op, std::string_view payload) {
    AppendFrameImpl(out, op, payload, /*masked=*/true);
}

}  // namespace bus::ws
