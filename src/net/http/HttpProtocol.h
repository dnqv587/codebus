//***********************************************
// name      : HttpProtocol.h
// brief     : HTTP 协议常量层：方法/状态码枚举（复用 3rd/http_parser 宏生成，
//             数值与 C 枚举恒等）及其字符串转换
// attention : 仅头文件；枚举生成依赖 3rd/http_parser.h（PUBLIC include）
//***********************************************
#pragma once
#include <cstdint>
#include <string_view>

#include "Macro.h"
#include "http_parser.h"

namespace bus::http {

/// @brief HTTP 方法枚举（数值与 ::http_method 恒等，static_cast 零成本互转）
enum class HttpMethod : uint8_t {
#define XX(num, name, string) k##name = num,
    HTTP_METHOD_MAP(XX)
#undef XX
};

/// @brief HTTP 状态码枚举（数值与 ::http_status 恒等）
enum class HttpStatus : uint16_t {
#define XX(num, name, string) k##name = num,
    HTTP_STATUS_MAP(XX)
#undef XX
};

/// @brief 方法名（"GET"/"POST"...）
/// @param method 方法枚举值
/// @return 大写字符串；非法值返回 "<unknown>"（C 库边界检查语义）
ATTR_NODISCARD
inline std::string_view HttpMethodToString(HttpMethod method) noexcept {
    return http_method_str(static_cast<http_method>(method));
}

/// @brief 状态码原因短语（"OK"/"Not Found"...）
/// @param status 状态码枚举值
/// @return 短语；非法值返回 "<unknown>"
ATTR_NODISCARD
inline std::string_view HttpStatusToString(HttpStatus status) noexcept {
    return http_status_str(static_cast<http_status>(status));
}

}  // namespace bus::http
