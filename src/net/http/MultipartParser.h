//***********************************************
// name      : MultipartParser.h
// brief     : multipart/form-data 纯函数解析（RFC 7578）+ boundary 提取
// attention : 无状态自由函数；body 整体受 HttpRequest maxBodyBytes 封顶，不做流式落盘
//***********************************************
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Macro.h"

namespace bus::http {

/// @brief multipart part：头域 + Content-Disposition name/filename + 原始字节 body
struct MultipartPart {
    /// @brief part 头域（键小写；保序）
    std::vector<std::pair<std::string, std::string>> headers;
    /// @brief Content-Disposition name 参数（无则空）
    std::string name;
    /// @brief Content-Disposition filename 参数（无则空）
    std::string filename;
    /// @brief part body 原始字节（二进制保真）
    std::string body;
};

/// @brief 从 Content-Type 提取 boundary 参数值（含引号包裹形态剥离）
/// @param content_type 原始 Content-Type 头值
/// @return boundary 值（不含 -- 前缀）；参数缺失返回 nullopt（媒体类型判定由调用方负责）
ATTR_NODISCARD
std::optional<std::string> ExtractBoundary(std::string_view content_type);

/// @brief 解析 multipart/form-data body
/// @param body 原始 body
/// @param boundary boundary（不含 -- 前缀）
/// @param out parts 输出（本函数先清空）
/// @return 成功 true；畸形 false（boundary 空/首分隔缺失/截断/非法分隔/缺头体空行）
ATTR_NODISCARD
bool ParseMultipart(std::string_view body, std::string_view boundary, std::vector<MultipartPart>& out);

}  // namespace bus::http