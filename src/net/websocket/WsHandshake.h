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

/// @brief 生成随机 Sec-WebSocket-Key（16 字节随机 → base64，24 字符；H5.2 客户端）
ATTR_NODISCARD
std::string GenerateSecKey();

/// @brief 构造客户端升级请求（GET + Upgrade/Connection/Key/Version 头；H5.2）
/// @param path 请求路径（含 query）
/// @param host Host 头值
/// @param key GenerateSecKey 产物
ATTR_NODISCARD
std::string BuildUpgradeRequest(std::string_view path, std::string_view host, std::string_view key);

/// @brief 校验 101 响应（H5.2 客户端：状态 101 + Upgrade websocket + Connection Upgrade + Accept 匹配）
/// @param resp 解析出的响应
/// @param key 请求发出的 Sec-WebSocket-Key
/// @param reason 失败原因输出（可空）
/// @return 合法 true
ATTR_NODISCARD
bool Validate101Response(const bus::http::HttpResponse& resp, std::string_view key, std::string* reason);

}  // namespace bus::ws
