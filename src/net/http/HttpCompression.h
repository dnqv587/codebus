//***********************************************
// name      : HttpCompression.h
// brief     : HTTP gzip 压缩纯函数层（RFC 1952 帧格式；zlib 系统库）
// attention : 解压输出上限封顶（zlib bomb 防护由调用方传 maxOut）
//***********************************************
#pragma once
#include <string>
#include <string_view>

#include "Macro.h"

namespace bus::http {

/// @brief gzip 压缩（RFC 1952 帧格式：windowBits=16+MAX_WBITS）
/// @param in 输入
/// @param out 压缩产物
/// @param level zlib 级别（-1=Z_DEFAULT_COMPRESSION；0-9）
/// @return 成功 true；zlib 失败 false+WARN（I1 不静默）
ATTR_NODISCARD
bool GzipCompress(std::string_view in, std::string& out, int level) noexcept;

/// @brief gzip 解压（流式 inflate）
/// @param in 压缩输入
/// @param out 解压产物
/// @param maxOut 输出上限（超限即停——zlib bomb 防护；HttpServer 侧传 maxBodyBytes）
/// @return 0=成功；-1=zlib 错误（非 gzip/截断/损坏）；-2=超上限（调用方回 413）
ATTR_NODISCARD
int GzipDecompress(std::string_view in, std::string& out, size_t maxOut) noexcept;

/// @brief Content-Type 是否可压缩（text/*、json、javascript、xml、svg）
ATTR_NODISCARD
bool IsCompressibleMime(std::string_view content_type) noexcept;

}  // namespace bus::http
