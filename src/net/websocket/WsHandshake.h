//***********************************************
// name      : WsHandshake.h
// brief     : WebSocket 握手（RFC 6455 §4）：SHA1+Base64 自实现（仅握手用途——
//             反跨协议指纹非安全原语，碰撞无关切）、升级请求校验、101 响应构造
// attention: SHA1/Base64 为 static 内部实现，不经此对外暴露
//***********************************************
#pragma once
#include <string>
#include <string_view>

#include "Macro.h"
#include "net/http/HttpMessage.h"

namespace bus::ws {

/// @brief SHA1 摘要（hex 小写输出——测试向量自检用）
ATTR_NODISCARD
std::string Sha1Hex(std::string_view input);

/// @brief 计算 Sec-WebSocket-Accept：Base64(SHA1(key + GUID))
/// @param key 请求的 Sec-WebSocket-Key（24 字符 base64）
/// @return 28 字符 accept 串
ATTR_NODISCARD
std::string ComputeAcceptKey(std::string_view key);

/// @brief 校验升级请求（Version==13 / key 为合法 base64（16 字节解码值）/ Connection 含 Upgrade）
/// @param req 升级请求（UpgradeHandler 收到的）
/// @param reason 失败原因输出（可空）
/// @return 合法 true
ATTR_NODISCARD
bool ValidateUpgradeRequest(const bus::http::HttpRequest& req, std::string* reason);

/// @brief 构造完整 101 切换协议响应
/// @param accept ComputeAcceptKey 产物
/// @return 响应串（以 \r\n\r\n 结尾）
ATTR_NODISCARD
std::string Build101Response(std::string_view accept);

}  // namespace bus::ws
