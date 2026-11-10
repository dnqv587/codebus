//***********************************************
// name      : WsSession.cc
// brief     : WebSocket 会话实现
// attention : None
//***********************************************
#include "net/websocket/WsSession.h"

#include "log/Logger.h"

namespace bus::ws {

static Logger* g_logger = GET_LOGGER("sys");

namespace {

/// @brief 有效 UTF-8 校验（增量式简化：整段校验——重组后调用）
bool IsValidUtf8(std::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t need = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            need = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + need >= s.size() + (need == 0 ? 1 : 0) && i + need > s.size() - 1) {
            if (i + need > s.size() - 1 && need > 0 && i + need + 1 > s.size()) {
                return false;  // 截断
            }
        }
        for (size_t k = 1; k <= need; ++k) {
            if (i + k >= s.size()) {
                return false;  // 截断
            }
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // 过长/代理区/超界
        if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += need + 1;
    }
    return true;
}

/// @brief 构造 close 帧（code+reason ≤125）
std::string MakeCloseFrame(uint16_t code, std::string_view reason) {
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xFF));
    if (reason.size() > 123) {
        reason = reason.substr(0, 123);
    }
    payload.append(reason);
    return payload;
}

}  // namespace

WsSession::WsSession(WsSessionConfig cfg)
    : m_cfg(cfg) {
}

ConsumeResult WsSession::Consume(const char* data, size_t len,
                                 const std::function<void(std::string_view)>& send) {
    m_input.append(data, len);
    while (true) {
        FrameHeader hdr;
        size_t consumed = 0;
        uint16_t close_code = 0;
        const ParseStatus st = ParseFrameHeader(m_input.data(), m_input.size(), &hdr, &consumed, &close_code);
        if (st == ParseStatus::kNeedMore) {
            return ConsumeResult::kContinue;
        }
        if (st == ParseStatus::kProtocolError) {
            if (!m_close_sent) {
                std::string frame;
                const std::string cp = MakeCloseFrame(close_code, "protocol error");
                AppendFrame(frame, Opcode::kClose, cp);
                send(frame);
                m_close_sent = true;
            }
            return ConsumeResult::kCloseSent;
        }
        if (m_input.size() - consumed < hdr.payloadLen) {
            return ConsumeResult::kContinue;  // payload 未齐
        }
        std::string payload = m_input.substr(consumed, hdr.payloadLen);
        ApplyMask(payload.data(), payload.size(), hdr.maskKey);
        m_input.erase(0, consumed + hdr.payloadLen);

        const ConsumeResult r = handleFrame(hdr, payload.c_str(), send);
        if (r == ConsumeResult::kCloseSent) {
            return r;
        }
    }
}

ConsumeResult WsSession::handleFrame(const FrameHeader& hdr, const char* raw,
                                     const std::function<void(std::string_view)>& send) {
    const std::string payload(raw, hdr.payloadLen);
    const auto op = hdr.opcode;

    if (op == Opcode::kClose) {
        uint16_t code = 1005;  // 无码
        if (payload.size() >= 2) {
            code = (static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]);
        }
        if (!m_close_sent) {  // echo close（RFC §5.5.1）
            std::string frame;
            const std::string cp = payload.empty() ? MakeCloseFrame(1000, "") : payload.substr(0, 2);
            AppendFrame(frame, Opcode::kClose, cp);
            send(frame);
            m_close_sent = true;
        }
        if (m_on_close) {
            m_on_close(code, payload.size() > 2 ? std::string_view(payload).substr(2) : "");
        }
        return ConsumeResult::kCloseSent;
    }
    if (op == Opcode::kPing) {
        std::string frame;  // 自动回 pong（RFC §5.5.2 强制；同负载）
        AppendFrame(frame, Opcode::kPong, payload);
        send(frame);
        return ConsumeResult::kContinue;
    }
    if (op == Opcode::kPong) {
        if (m_on_pong) {
            m_on_pong();  // 心跳应答记账（WsServer sweep 清 ping 死线；主动 pong 同样触发）
        }
        return ConsumeResult::kContinue;
    }

    // 数据帧
    if (op == Opcode::kText || op == Opcode::kBinary) {
        if (m_fragging) {
            if (!m_close_sent) {  // 新数据帧打断分片 → 违规
                std::string frame;
                AppendFrame(frame, Opcode::kClose, MakeCloseFrame(1002, "unexpected new data frame"));
                send(frame);
                m_close_sent = true;
            }
            return ConsumeResult::kCloseSent;
        }
        m_frag_text = (op == Opcode::kText);
        m_frag.assign(payload);
        if (hdr.fin) {
            deliverMessage(send);
            return ConsumeResult::kContinue;
        }
        m_fragging = true;
        return ConsumeResult::kContinue;
    }
    // kContinuation
    if (!m_fragging) {
        if (!m_close_sent) {
            std::string frame;
            AppendFrame(frame, Opcode::kClose, MakeCloseFrame(1002, "continuation without start"));
            send(frame);
            m_close_sent = true;
        }
        return ConsumeResult::kCloseSent;
    }
    if (m_frag.size() + payload.size() > m_cfg.maxMessageBytes) {  // 分片炸弹
        if (!m_close_sent) {
            std::string frame;
            AppendFrame(frame, Opcode::kClose, MakeCloseFrame(1009, "message too large"));
            send(frame);
            m_close_sent = true;
        }
        return ConsumeResult::kCloseSent;
    }
    m_frag.append(payload);
    if (hdr.fin) {
        m_fragging = false;
        deliverMessage(send);
    }
    return ConsumeResult::kContinue;
}

void WsSession::deliverMessage(const std::function<void(std::string_view)>& send) {
    if (m_cfg.validateText && m_frag_text && !IsValidUtf8(m_frag)) {
        if (!m_close_sent) {
            std::string frame;
            AppendFrame(frame, Opcode::kClose, MakeCloseFrame(1007, "invalid utf-8"));
            send(frame);
            m_close_sent = true;
        }
        return;
    }
    if (m_on_message) {
        WsMessage msg;
        msg.isText = m_frag_text;
        msg.payload = std::move(m_frag);
        m_frag.clear();
        m_on_message(msg);
    }
}

void WsSession::sendText(std::string_view text, const std::function<void(std::string_view)>& send) {
    std::string frame;
    AppendFrame(frame, Opcode::kText, text);
    send(frame);
}

void WsSession::sendBinary(std::string_view data, const std::function<void(std::string_view)>& send) {
    std::string frame;
    AppendFrame(frame, Opcode::kBinary, data);
    send(frame);
}

void WsSession::ping(const std::function<void(std::string_view)>& send) {
    std::string frame;
    AppendFrame(frame, Opcode::kPing, "hb");
    send(frame);
}

void WsSession::close(uint16_t code, std::string_view reason,
                      const std::function<void(std::string_view)>& send) {
    if (m_close_sent) {
        return;
    }
    std::string frame;
    AppendFrame(frame, Opcode::kClose, MakeCloseFrame(code, reason));
    send(frame);
    m_close_sent = true;
}

}  // namespace bus::ws
