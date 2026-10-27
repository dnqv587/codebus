//***********************************************
// name      : HttpUtil.h
// brief     : HTTP 纯函数工具：URL 编解码、query 解析、ASCII 大小写不敏感比较
// attention : 均为无状态自由函数；URL 解码对非法 % 序列宽容保留字面
//***********************************************
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Macro.h"

namespace bus::http {

/// @brief URL 解码（%XX 转义还原）
/// @param in 输入串
/// @param plusAsSpace true 时 '+' 解码为空格（仅 query 场景；path 中 '+' 为字面量）
/// @return 解码结果；非法 % 序列（后随非两位十六进制）宽容保留字面字符
ATTR_NODISCARD
std::string UrlDecode(std::string_view in, bool plusAsSpace) noexcept;

/// @brief URL 编码（RFC 3986 unreserved 之外的字符转 %XX，空格转 %20）
/// @param in 输入串
/// @return 编码结果
ATTR_NODISCARD
std::string UrlEncode(std::string_view in) noexcept;

/// @brief 解析 query 串（a=1&b=2 形态；key/value 均已解码且 '+' 按空格）
/// @param raw query 原始串（不含 '?'）
/// @param out 输出参数对（本函数先清空）
/// @return 至少解析出一对返回 true；空串/全空段返回 false
ATTR_NODISCARD
bool ParseQuery(std::string_view raw, std::vector<std::pair<std::string, std::string>>& out) noexcept;

/// @brief ASCII 大小写不敏感比较
/// @return 逐字符 fold 后相等（长度不等必 false）
ATTR_NODISCARD
bool EqualCI(std::string_view a, std::string_view b) noexcept;

}  // namespace bus::http
