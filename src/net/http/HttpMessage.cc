//***********************************************
// name      : HttpMessage.cc
// brief     : HTTP 协议对象层实现
// attention : None
//***********************************************
#include "net/http/HttpMessage.h"

#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log/Logger.h"
#include "net/http/HttpUtil.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");

namespace {

/// @brief RFC 1123 GMT 时间串（"Tue, 15 Nov 1994 08:12:31 GMT"；gmtime_r 线程安全）
inline std::string HttpDateNow() {
    // 查表生成星期/月份缩写：strftime %a/%b 依赖 LC_TIME locale（Q33）
    static const char* kWeek[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonth[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    ::time_t now = ::time(nullptr);
    struct tm tm_gmt;
    gmtime_r(&now, &tm_gmt);
    snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT", kWeek[tm_gmt.tm_wday],
             tm_gmt.tm_mday, kMonth[tm_gmt.tm_mon], tm_gmt.tm_year + 1900, tm_gmt.tm_hour,
             tm_gmt.tm_min, tm_gmt.tm_sec);
    return buf;
}

/// @brief Dump 托管头集合（框架独占输出，用户设置被跳过）
inline bool IsManagedHeader(std::string_view name) noexcept {
    return EqualCI(name, "content-length") || EqualCI(name, "connection") || EqualCI(name, "transfer-encoding") ||
           EqualCI(name, "server") || EqualCI(name, "set-cookie") || EqualCI(name, "date");
}

/// @brief 头名/值安全检查（拒绝 CR/LF/NUL——防响应拆分/CRLF 注入；请求侧头经
///        http_parser 已滤，此处为响应侧防线，下沉 HeaderMap 覆盖全部写路径）
inline bool IsHeaderSafe(std::string_view s) noexcept {
    return s.find_first_of("\r\n\0", 0) == std::string_view::npos;  // NOLINT: 显式查 NUL
}

/// @brief 1xx/204/304 按协议不携带 body
inline bool StatusOmitsBody(HttpStatus s) noexcept {
    const int v = static_cast<int>(s);
    return (v >= 100 && v < 200) || v == 204 || v == 304;
}

/// @brief 去除两端空格（含 \t）
inline std::string_view TrimSpace(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

// ---------------- HeaderMap ----------------

std::optional<std::string_view> HeaderMap::getHeader(std::string_view name) const noexcept {
    for (const auto& [k, v] : m_headers) {
        if (EqualCI(k, name)) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> HeaderMap::getHeaders(std::string_view name) const {
    std::vector<std::string_view> out;
    for (const auto& [k, v] : m_headers) {
        if (EqualCI(k, name)) {
            out.emplace_back(v);
        }
    }
    return out;
}

void HeaderMap::setHeader(std::string_view name, std::string_view value) {
    if (!IsHeaderSafe(name) || !IsHeaderSafe(value)) {
        LOG_FMT_WARN(g_logger, "HeaderMap::setHeader: CR/LF/NUL in '%.*s' rejected (header injection guard)",
                     static_cast<int>(name.size()), name.data());
        return;  // 整条拒绝（截断会留下语义不完整的半值，更危险）
    }
    m_headers.erase(
        std::remove_if(m_headers.begin(), m_headers.end(), [&](const auto& kv) { return EqualCI(kv.first, name); }),
        m_headers.end());
    m_headers.emplace_back(name, value);
}

void HeaderMap::addHeader(std::string_view name, std::string_view value) {
    if (!IsHeaderSafe(name) || !IsHeaderSafe(value)) {
        LOG_FMT_WARN(g_logger, "HeaderMap::addHeader: CR/LF/NUL in '%.*s' rejected (header injection guard)",
                     static_cast<int>(name.size()), name.data());
        return;
    }
    m_headers.emplace_back(name, value);
}

bool HeaderMap::hasHeader(std::string_view name) const noexcept {
    return getHeader(name).has_value();
}

void HeaderMap::clear() noexcept {
    m_headers.clear();
}

void HeaderMap::Swap(HeaderMap& o) noexcept {
    m_headers.swap(o.m_headers);
}

// ---------------- Cookie ----------------

std::vector<Cookie> ParseRequestCookies(std::string_view header) noexcept {
    std::vector<Cookie> out;
    size_t start = 0;
    while (start < header.size()) {
        size_t end = header.find(';', start);
        if (end == std::string_view::npos) {
            end = header.size();
        }
        std::string_view seg = TrimSpace(header.substr(start, end - start));
        if (!seg.empty()) {
            size_t eq = seg.find('=');
            if (eq != std::string_view::npos && eq > 0) {
                Cookie c;
                c.name = TrimSpace(seg.substr(0, eq));
                c.value = TrimSpace(seg.substr(eq + 1));
                if (!c.name.empty()) {
                    out.push_back(std::move(c));
                }
            }
        }
        if (end == header.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::string SerializeSetCookie(const Cookie& c) noexcept {
    std::string out;
    out.reserve(c.name.size() + c.value.size() + 64);
    out += c.name;
    out += '=';
    out += c.value;
    if (!c.path.empty()) {
        out += "; Path=";
        out += c.path;
    }
    if (!c.domain.empty()) {
        out += "; Domain=";
        out += c.domain;
    }
    if (!c.expires.empty()) {
        out += "; Expires=";
        out += c.expires;
    }
    if (c.maxAge >= 0) {
        out += "; Max-Age=";
        out += std::to_string(c.maxAge);
    }
    if (!c.sameSite.empty()) {
        out += "; SameSite=";
        out += c.sameSite;
    }
    if (c.secure) {
        out += "; Secure";
    }
    if (c.httpOnly) {
        out += "; HttpOnly";
    }
    return out;
}

// ---------------- HttpRequest ----------------

std::string HttpRequest::getVersion() const noexcept {
    return "HTTP/" + std::to_string(m_major) + "." + std::to_string(m_minor);
}

std::optional<std::string_view> HttpRequest::getParam(std::string_view key) const {
    for (const auto& [k, v] : m_params) {
        if (k == key) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> HttpRequest::getParams(std::string_view key) const {
    std::vector<std::string_view> out;
    for (const auto& [k, v] : m_params) {
        if (k == key) {
            out.emplace_back(v);
        }
    }
    return out;
}

std::optional<std::string_view> HttpRequest::getCookie(std::string_view name) const {
    for (const Cookie& c : m_cookies) {
        if (c.name == name) {
            return std::string_view(c.value);
        }
    }
    return std::nullopt;
}

void HttpRequest::Swap(HttpRequest& o) noexcept {
    std::swap(m_method, o.m_method);
    std::swap(m_major, o.m_major);
    std::swap(m_minor, o.m_minor);
    m_raw_url.swap(o.m_raw_url);
    m_path.swap(o.m_path);
    m_query.swap(o.m_query);
    m_headers.Swap(o.m_headers);
    m_params.swap(o.m_params);
    m_form_params.swap(o.m_form_params);
    m_path_params.swap(o.m_path_params);
    m_remote_addr.swap(o.m_remote_addr);
    m_cookies.swap(o.m_cookies);
    m_body.swap(o.m_body);
    std::swap(m_keep_alive, o.m_keep_alive);
    std::swap(m_chunked, o.m_chunked);
    std::swap(m_expect_continue, o.m_expect_continue);
}

std::string HttpRequest::Dump(std::string_view host) const {
    std::string out;
    out.reserve(128 + m_headers.size() * 32 + m_body.size());
    out += HttpMethodToString(m_method);
    out += ' ';
    out += (m_raw_url.empty() ? m_path : m_raw_url);
    out += " HTTP/";
    out += std::to_string(m_major);
    out += '.';
    out += std::to_string(m_minor);
    out += "\r\n";
    if (!getHeader("host").has_value()) {
        out += "Host: ";
        out += host;
        out += "\r\n";
    }
    for (const auto& [name, value] : m_headers) {
        if (EqualCI(name, "host") || EqualCI(name, "content-length") || EqualCI(name, "connection")) {
            continue;  // 托管头
        }
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(m_body.size());
    out += "\r\nConnection: ";
    out += m_keep_alive ? "keep-alive" : "close";
    out += "\r\n\r\n";
    if (!m_body.empty()) {
        out += m_body;
    }
    return out;
}

void HttpRequest::finishParams() {
    m_params.clear();
    ParseQuery(m_query, m_params);
}

void HttpRequest::finishForm() {
    m_form_params.clear();
    m_path_params.clear();
    m_remote_addr.clear();
    auto ct = getHeader("content-type");
    if (ct && EqualCI(ct->substr(0, 33), "application/x-www-form-urlencoded")) {
        ParseQuery(m_body, m_form_params);
    }
}

void HttpRequest::finishCookies() {
    m_cookies.clear();
    // RFC 允许多个 Cookie 头分段发送：逐个解析合并
    for (std::string_view h : m_headers.getHeaders("cookie")) {
        std::vector<Cookie> part = ParseRequestCookies(h);
        m_cookies.insert(m_cookies.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
}

std::optional<std::string_view> HttpRequest::getPathParam(std::string_view name) const {
    for (const auto& [k, v] : m_path_params) {
        if (k == name) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::string_view HttpRequest::getRemoteAddr() const noexcept {
    return m_remote_addr;
}

void HttpRequest::setRemoteAddr(std::string addr) const {
    m_remote_addr = std::move(addr);
}

void HttpRequest::setBodyDecompressed(std::string plain) const {
    m_body = std::move(plain);
}

void HttpRequest::setPathParam(std::string_view name, std::string_view value) const {
    m_path_params.emplace_back(name, value);
}

std::optional<std::string_view> HttpRequest::getFormParam(std::string_view key) const {
    for (const auto& [k, v] : m_form_params) {
        if (k == key) {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> HttpRequest::getFormParams(std::string_view key) const {
    std::vector<std::string_view> out;
    for (const auto& [k, v] : m_form_params) {
        if (k == key) {
            out.emplace_back(v);
        }
    }
    return out;
}

void HttpRequest::clear() noexcept {
    m_method = HttpMethod::kGET;
    m_major = 1;
    m_minor = 1;
    m_raw_url.clear();
    m_path.clear();
    m_query.clear();
    m_headers.clear();
    m_params.clear();
    m_form_params.clear();
    m_path_params.clear();
    m_remote_addr.clear();
    m_cookies.clear();
    m_body.clear();
    m_keep_alive = true;
    m_chunked = false;
    m_expect_continue = false;
}

// ---------------- HttpResponse ----------------

HttpResponse::HttpResponse(std::string serverName) : m_server_name(std::move(serverName)) {}

void HttpResponse::SendError(HttpStatus s, std::string_view msg) {
    m_status = s;
    m_headers.setHeader("Content-Type", "text/plain; charset=utf-8");
    m_body = msg.empty() ? std::to_string(static_cast<uint16_t>(s)) + " " + std::string(HttpStatusToString(s))
                         : std::string(msg);
}

void HttpResponse::SendRedirect(std::string_view location) {
    m_status = HttpStatus::kFOUND;  // 302
    m_headers.setHeader("Location", location);
    m_headers.setHeader("Content-Type", "text/html; charset=utf-8");
    m_body = "<html><body>Redirecting to <a href=\"" + std::string(location) + "\">" + std::string(location) +
             "</a></body></html>";
}

std::string HttpResponse::DumpHead(const HttpRequest& req) const {
    const bool is_head = (req.getMethod() == HttpMethod::kHEAD);
    const bool omit_body = is_head || StatusOmitsBody(m_status);
    const bool keep_alive = req.isKeepAlive() && m_keep_alive;

    std::string out;
    out.reserve(128 + m_headers.size() * 32 + m_cookies.size() * 48 + m_body.size());

    // 状态行：HTTP 版本跟随请求（major/minor 均取自请求）
    out += "HTTP/";
    out += std::to_string(req.getHttpMajor());
    out += '.';
    out += std::to_string(req.getHttpMinor());
    out += ' ';
    out += std::to_string(static_cast<uint16_t>(m_status));
    out += ' ';
    out += HttpStatusToString(m_status);
    out += "\r\n";

    out += "Server: ";
    out += m_server_name;
    out += "\r\nDate: ";
    out += HttpDateNow();
    out += "\r\n";

    // 用户头（插入序）；托管头跳过 + WARN
    for (const auto& [name, value] : m_headers) {
        if (IsManagedHeader(name)) {
            LOG_FMT_WARN(g_logger, "HttpResponse::Dump: user header '%s' is managed by framework, skipped",
                         name.c_str());
            continue;
        }
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }

    // Set-Cookie 每条独立行；注入防线覆盖全部属性段（Q39：name/value 之外的
    // path/domain/expires/sameSite 由用户直塞 Cookie 结构时同样存在注入面）
    for (const Cookie& c : m_cookies) {
        if (!IsHeaderSafe(c.name) || !IsHeaderSafe(c.value) || !IsHeaderSafe(c.path) ||
            !IsHeaderSafe(c.domain) || !IsHeaderSafe(c.expires) || !IsHeaderSafe(c.sameSite)) {
            LOG_FMT_WARN(g_logger, "HttpResponse::Dump: cookie '%s' rejected (injection guard, all attrs)",
                         c.name.c_str());
            continue;
        }
        out += "Set-Cookie: ";
        out += SerializeSetCookie(c);
        out += "\r\n";
    }

    // Content-Length：文件源优先（流式首包）；1xx/204 按协议禁止携带（RFC 7230 §3.3.2）
    const int code = static_cast<int>(m_status);
    const bool omit_cl = (code >= 100 && code < 200) || code == 204;
    const bool http10 = (req.getHttpMajor() == 1 && req.getHttpMinor() == 0);
    if (m_provider != nullptr) {
        // provider 流式源：1.1 内建 chunked（无 CL）；1.0 close-delimited（无 TE 无 CL）
        if (!http10) {
            out += "Transfer-Encoding: chunked\r\n";
        }
    } else if (!omit_cl) {
        out += "Content-Length: ";
        out += std::to_string(m_body_file != nullptr ? m_body_file_size : m_body.size());
        out += "\r\n";
    }
    out += "Connection: ";
    out += keep_alive ? "keep-alive" : "close";
    out += "\r\n\r\n";
    return out;
}

std::string HttpResponse::Dump(const HttpRequest& req) const {
    std::string out = DumpHead(req);
    const bool is_head = (req.getMethod() == HttpMethod::kHEAD);
    const int code = static_cast<int>(m_status);
    const bool omit_body = is_head || (code >= 100 && code < 200) || code == 204 || code == 304;
    if (!omit_body && m_body_file == nullptr && !m_body.empty()) {
        out += m_body;
    }
    return out;  // 文件源：body 字节由 HttpServer 分块发送（Dump 不内联）
}

bool HttpResponse::setBodyFile(std::string_view path) {
    struct ::stat st;
    std::string p(path);
    if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    m_body_file = std::shared_ptr<int>(new int(fd), [](int* p) {
        if (p != nullptr) {
            ::close(*p);
            delete p;
        }
    });
    m_body_file_size = static_cast<size_t>(st.st_size);
    return true;
}

int HttpResponse::bodyFileFd() const noexcept {
    return m_body_file ? *m_body_file : -1;
}

bool HttpResponse::setBodyFileRange(std::string_view path, uint64_t offset, uint64_t count) {
    struct ::stat st;
    std::string p(path);
    if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || count == 0 ||
        offset >= static_cast<uint64_t>(st.st_size)) {
        return false;
    }
    if (!setBodyFile(path)) {
        return false;
    }
    const uint64_t avail = static_cast<uint64_t>(st.st_size) - offset;
    m_body_file_offset = offset;
    m_body_file_size = static_cast<size_t>(std::min<uint64_t>(count, avail));
    return true;
}

}  // namespace bus::http
