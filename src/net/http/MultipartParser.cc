//***********************************************
// name      : MultipartParser.cc
// brief     : multipart/form-data 纯函数解析实现
// attention : 严格分隔语法（首分隔必须位于 body 起点；闭合分隔 --boundary--）
//***********************************************
#include "net/http/MultipartParser.h"

#include <cctype>

namespace bus::http {

namespace {

/// @brief 剥离首尾空白
std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

/// @brief ASCII 小写化
std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/// @brief 从 Content-Disposition 值提取引号参数（name="..." / filename="..."）
/// @attention 参数边界检查：needle 前一字符须为起点/;/空白——裸 find 会把
///            filename=" 内部子串误匹配为 name="（审查修复）
std::string ExtractQuotedParam(std::string_view disp, std::string_view key) {
    const std::string needle = std::string(key) + "=\"";
    size_t pos = 0;
    for (;;) {
        pos = disp.find(needle, pos);
        if (pos == std::string_view::npos) {
            return {};
        }
        const bool at_boundary = pos == 0 || disp[pos - 1] == ';' || disp[pos - 1] == ' ' || disp[pos - 1] == '\t';
        pos += needle.size();
        if (at_boundary) {
            size_t end = disp.find('"', pos);
            if (end == std::string_view::npos) {
                return {};
            }
            return std::string(disp.substr(pos, end - pos));
        }
    }
}

}  // namespace

std::optional<std::string> ExtractBoundary(std::string_view content_type) {
    size_t pos = 0;
    while (pos < content_type.size()) {
        size_t semi = content_type.find(';', pos);
        std::string_view param = Trim(content_type.substr(
            pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos));
        size_t eq = param.find('=');
        if (eq != std::string_view::npos) {
            std::string_view key = Trim(param.substr(0, eq));
            std::string_view val = Trim(param.substr(eq + 1));
            if (Lower(key) == "boundary") {
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);  // 引号包裹形态
                }
                if (val.empty()) {
                    return std::nullopt;
                }
                return std::string(val);
            }
        }
        if (semi == std::string_view::npos) {
            break;
        }
        pos = semi + 1;
    }
    return std::nullopt;
}

bool ParseMultipart(std::string_view body, std::string_view boundary, std::vector<MultipartPart>& out) {
    out.clear();
    if (boundary.empty()) {
        return false;
    }
    const std::string delim = "--" + std::string(boundary);
    if (body.rfind(delim, 0) != 0) {
        return false;  // 首分隔必须位于起点（不接受 preamble）
    }
    size_t pos = delim.size();
    const std::string next_delim = "\r\n" + delim;
    for (;;) {
        if (body.substr(pos, 2) == "--") {
            return true;  // 闭合分隔（其后 CRLF 可选）
        }
        if (body.substr(pos, 2) != "\r\n") {
            return false;  // 非法分隔
        }
        pos += 2;
        size_t next = body.find(next_delim, pos);
        if (next == std::string_view::npos) {
            return false;  // 截断
        }
        const std::string_view part = body.substr(pos, next - pos);
        MultipartPart p;
        const size_t hdr_end = part.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos) {
            // 空 body part：空行的首个 CRLF 留在 part 末尾（次个被下一分隔消费）；
            // 缺尾 CRLF = 头域直接接分隔（无空行）→ 畸形（审查修复，RFC 7578 要求空行）
            if (part.size() < 2 || part.substr(part.size() - 2) != "\r\n") {
                return false;
            }
        }
        const std::string_view headers =
            hdr_end == std::string_view::npos ? part.substr(0, part.size() - 2) : part.substr(0, hdr_end);
        if (hdr_end != std::string_view::npos) {
            p.body = std::string(part.substr(hdr_end + 4));
        }
        size_t hpos = 0;
        while (hpos < headers.size()) {
            size_t eol = headers.find("\r\n", hpos);
            std::string_view line =
                headers.substr(hpos, eol == std::string_view::npos ? std::string_view::npos : eol - hpos);
            size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string key = Lower(Trim(line.substr(0, colon)));
                std::string_view val = Trim(line.substr(colon + 1));
                if (key == "content-disposition") {
                    p.name = ExtractQuotedParam(val, "name");
                    p.filename = ExtractQuotedParam(val, "filename");
                }
                p.headers.emplace_back(std::move(key), std::string(val));
            }
            if (eol == std::string_view::npos) {
                break;
            }
            hpos = eol + 2;
        }
        out.push_back(std::move(p));
        pos = next + next_delim.size();
    }
}

}  // namespace bus::http