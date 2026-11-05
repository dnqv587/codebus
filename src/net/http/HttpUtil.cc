//***********************************************
// name      : HttpUtil.cc
// brief     : HTTP 纯函数工具实现
// attention : None
//***********************************************
#include "net/http/HttpUtil.h"

#include <atomic>
#include <optional>

#include "log/Logger.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("sys");

namespace {

/// @brief 十六进制字符转数值，非法返回 -1
inline int HexValue(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// @brief 数值转大写十六进制字符（0-15）
inline char ToHexChar(int v) noexcept {
    return static_cast<char>(v < 10 ? '0' + v : 'A' + v - 10);
}

/// @brief RFC 3986 unreserved 集合（ALPHA / DIGIT / '-' / '.' / '_' / '~'）
inline bool IsUnreserved(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

}  // namespace

std::string UrlDecode(std::string_view in, bool plusAsSpace) noexcept {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+' && plusAsSpace) {
            out.push_back(' ');
            continue;
        }
        if (c == '%' && i + 2 < in.size()) {
            int hi = HexValue(in[i + 1]);
            int lo = HexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
            /// 非法 % 序列：宽容保留字面（DEBUG 计数汇总防日志洪水——Q35：首条+每 1000 条）
            static std::atomic<uint64_t> s_invalid_total{0};
            uint64_t n_inv = ++s_invalid_total;
            if (n_inv == 1 || n_inv % 1000 == 0) {
                LOG_FMT_DEBUG(g_logger, "UrlDecode: invalid escape (total %llu, last at %zu)",
                              static_cast<unsigned long long>(n_inv), i);
            }
        }
        out.push_back(c);
    }
    return out;
}

std::string UrlEncode(std::string_view in) noexcept {
    std::string out;
    out.reserve(in.size() * 3);
    for (char ch : in) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (IsUnreserved(c)) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(ToHexChar(c >> 4));
            out.push_back(ToHexChar(c & 0xF));
        }
    }
    return out;
}

bool ParseQuery(std::string_view raw, std::vector<std::pair<std::string, std::string>>& out) noexcept {
    out.clear();
    if (raw.empty()) {
        return false;
    }
    size_t start = 0;
    while (start <= raw.size()) {
        size_t end = raw.find('&', start);
        if (end == std::string_view::npos) {
            end = raw.size();
        }
        std::string_view seg = raw.substr(start, end - start);
        if (!seg.empty()) {
            size_t eq = seg.find('=');
            if (eq == std::string_view::npos) {
                out.emplace_back(UrlDecode(seg, true), std::string());
            } else {
                out.emplace_back(UrlDecode(seg.substr(0, eq), true), UrlDecode(seg.substr(eq + 1), true));
            }
        }
        if (end == raw.size()) {
            break;
        }
        start = end + 1;
    }
    return !out.empty();
}

std::optional<std::string> NormalizePath(std::string_view path) noexcept {
    std::vector<std::string_view> stack;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        std::string_view seg = path.substr(start, end - start);
        if (seg == "..") {
            if (stack.empty()) {
                return std::nullopt;  // 越界：路径逃逸根
            }
            stack.pop_back();
        } else if (!seg.empty() && seg != ".") {
            stack.push_back(seg);
        }
        if (end == path.size()) {
            break;
        }
        start = end + 1;
    }
    std::string out;
    out.reserve(path.size());
    for (std::string_view seg : stack) {
        out += '/';
        out.append(seg);
    }
    if (out.empty()) {
        out = "/";
    }
    return out;
}

bool EqualCI(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y - 'A' + 'a');
        if (x != y) {
            return false;
        }
    }
    return true;
}

}  // namespace bus::http
