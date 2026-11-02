//***********************************************
// name      : WsFrame.h
// brief     : WebSocket 帧编解码纯函数（RFC 6455 §5）：头解析三态/服务端协议规则
//             全表（1002 各情形）/掩码 XOR/服务端帧追加（永不掩码）
// attention : 纯函数无状态；仅服务端视角（客户端帧必须掩码）
//***********************************************
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "Macro.h"

namespace bus::ws {

/// @brief 帧操作码（RFC 6455 §5.2）
enum class Opcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

/// @brief 帧头解析结果
enum class ParseStatus {
    kNeedMore,       ///< 数据不足一字节/一帧
    kOk,             ///< 解析成功（payload 紧随 consumed 之后）
    kProtocolError,  ///< 协议违规（close_code 已填——调用方应回 close 帧）
};

/// @brief 帧头（解析产物）
struct FrameHeader {
    /// @brief FIN 位（消息末帧）
    bool fin = false;
    /// @brief 操作码
    Opcode opcode = Opcode::kContinuation;
    /// @brief 掩码位（客户端→服务端必须为 1）
    bool masked = false;
    /// @brief 负载长度
    uint64_t payloadLen = 0;
    /// @brief 掩码键（masked 时有效）
    uint8_t maskKey[4] = {0, 0, 0, 0};
};

/// @brief 解析帧头（服务端规则内建：未掩码/RSV≠0/控制帧分片或>125/未知 opcode → kProtocolError+1002）
/// @param data 输入
/// @param len 字节数
/// @param hdr 输出帧头
/// @param consumed 输出已消费字节数（含头与扩展长度；kOk 时 payload 自此起）
/// @param close_code kProtocolError 时的关闭码
ATTR_NODISCARD
ParseStatus ParseFrameHeader(const char* data, size_t len, FrameHeader* hdr, size_t* consumed,
                             uint16_t* close_code);

/// @brief 掩码 XOR（双向幂等；payload 就地变换）
void ApplyMask(char* data, size_t len, const uint8_t key[4]) noexcept;

/// @brief 追加一个完整服务端帧（FIN=1；服务端→客户端帧永不掩码——RFC §5.1）
/// @param out 输出串追加
/// @param op 操作码
/// @param payload 负载（控制帧 ≤125 由调用方保证——AppendFrame 不再校验）
void AppendFrame(std::string& out, Opcode op, std::string_view payload);

}  // namespace bus::ws
