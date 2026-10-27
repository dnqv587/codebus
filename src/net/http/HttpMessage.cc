//***********************************************
// name      : HttpMessage.cc
// brief     : HTTP 协议对象层实现
// attention : None
//***********************************************
#include "net/http/HttpMessage.h"

#include <algorithm>

#include "log/Logger.h"
#include "net/http/HttpUtil.h"

namespace bus::http
{

static Logger* g_logger = GET_LOGGER("system");

namespace {

/// @brief RFC 1123 GMT 时间串（"Tue, 15 Nov 1994 08:12:31 GMT"；gmtime_r 线程安全）
inline std::string HttpDateNow()
{
    char buf[64];
    ::time_t now = ::time(nullptr);
    struct tm tm_gmt;
    gmtime_r(&now, &tm_gmt);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt);
    return buf;
}

/// @brief Dump 托管头集合（框架独占输出，用户设置被跳过）
inline bool IsManagedHeader(std::string_view name) noexcept
{
    return EqualCI(name, "content-length") || EqualCI(name, "connection") ||
           EqualCI(name, "transfer-encoding") || EqualCI(name, "server") ||
           EqualCI(name, "set-cookie") || EqualCI(name, "date");
}

/// @brief 1xx/204/304 按协议不携带 body
inline bool StatusOmitsBody(HttpStatus s) noexcept
{
    const int v = static_cast<int>(s);
    return (v >= 100 && v < 200) || v == 204 || v == 304;
}

/// @brief 去除两端空格（含 \t）
inline std::string_view TrimSpace(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

// ---------------- HeaderMap ----------------

std::optional<std::string_view> HeaderMap::getHeader(std::string_view name) const noexcept
{
    for (const auto& [k, v] : m_headers)
    {
        if (EqualCI(k, name))
        {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> HeaderMap::getHeaders(std::string_view name) const
{
    std::vector<std::string_view> out;
    for (const auto& [k, v] : m_headers)
    {
        if (EqualCI(k, name))
        {
            out.emplace_back(v);
        }
    }
    return out;
}

void HeaderMap::setHeader(std::string_view name, std::string_view value)
{
    m_headers.erase(std::remove_if(m_headers.begin(), m_headers.end(),
                                   [&](const auto& kv)
                                   { return EqualCI(kv.first, name); }),
                    m_headers.end());
    m_headers.emplace_back(name, value);
}

void HeaderMap::addHeader(std::string_view name, std::string_view value)
{
    m_headers.emplace_back(name, value);
}

bool HeaderMap::hasHeader(std::string_view name) const noexcept
{
    return getHeader(name).has_value();
}

void HeaderMap::clear() noexcept
{
    m_headers.clear();
}

void HeaderMap::Swap(HeaderMap& o) noexcept
{
    m_headers.swap(o.m_headers);
}

// ---------------- Cookie ----------------

std::vector<Cookie> ParseRequestCookies(std::string_view header) noexcept
{
    std::vector<Cookie> out;
    size_t start = 0;
    while (start < header.size())
    {
        size_t end = header.find(';', start);
        if (end == std::string_view::npos)
        {
            end = header.size();
        }
        std::string_view seg = TrimSpace(header.substr(start, end - start));
        if (!seg.empty())
        {
            size_t eq = seg.find('=');
            if (eq != std::string_view::npos && eq > 0)
            {
                Cookie c;
                c.name = TrimSpace(seg.substr(0, eq));
                c.value = TrimSpace(seg.substr(eq + 1));
                if (!c.name.empty())
                {
                    out.push_back(std::move(c));
                }
            }
        }
        if (end == header.size())
        {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::string SerializeSetCookie(const Cookie& c) noexcept
{
    std::string out;
    out.reserve(c.name.size() + c.value.size() + 64);
    out += c.name;
    out += '=';
    out += c.value;
    if (!c.path.empty())
    {
        out += "; Path=";
        out += c.path;
    }
    if (!c.domain.empty())
    {
        out += "; Domain=";
        out += c.domain;
    }
    if (!c.expires.empty())
    {
        out += "; Expires=";
        out += c.expires;
    }
    if (c.maxAge >= 0)
    {
        out += "; Max-Age=";
        out += std::to_string(c.maxAge);
    }
    if (!c.sameSite.empty())
    {
        out += "; SameSite=";
        out += c.sameSite;
    }
    if (c.secure)
    {
        out += "; Secure";
    }
    if (c.httpOnly)
    {
        out += "; HttpOnly";
    }
    return out;
}

// ---------------- HttpRequest ----------------

std::string HttpRequest::getVersion() const noexcept
{
    return "HTTP/" + std::to_string(m_major) + "." + std::to_string(m_minor);
}

std::optional<std::string_view> HttpRequest::getParam(std::string_view key) const
{
    for (const auto& [k, v] : m_params)
    {
        if (k == key)
        {
            return std::string_view(v);
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> HttpRequest::getParams(std::string_view key) const
{
    std::vector<std::string_view> out;
    for (const auto& [k, v] : m_params)
    {
        if (k == key)
        {
            out.emplace_back(v);
        }
    }
    return out;
}

std::optional<std::string_view> HttpRequest::getCookie(std::string_view name) const
{
    for (const Cookie& c : m_cookies)
    {
        if (c.name == name)
        {
            return std::string_view(c.value);
        }
    }
    return std::nullopt;
}

void HttpRequest::Swap(HttpRequest& o) noexcept
{
    std::swap(m_method, o.m_method);
    std::swap(m_major, o.m_major);
    std::swap(m_minor, o.m_minor);
    m_raw_url.swap(o.m_raw_url);
    m_path.swap(o.m_path);
    m_query.swap(o.m_query);
    m_headers.Swap(o.m_headers);
    m_params.swap(o.m_params);
    m_cookies.swap(o.m_cookies);
    m_body.swap(o.m_body);
    std::swap(m_keep_alive, o.m_keep_alive);
    std::swap(m_chunked, o.m_chunked);
    std::swap(m_expect_continue, o.m_expect_continue);
}

void HttpRequest::finishParams()
{
    m_params.clear();
    ParseQuery(m_query, m_params);
}

void HttpRequest::finishCookies()
{
    auto cookie_header = getHeader("cookie");
    m_cookies = cookie_header ? ParseRequestCookies(*cookie_header) : std::vector<Cookie>{};
}

void HttpRequest::clear() noexcept
{
    m_method = HttpMethod::kGET;
    m_major = 1;
    m_minor = 1;
    m_raw_url.clear();
    m_path.clear();
    m_query.clear();
    m_headers.clear();
    m_params.clear();
    m_cookies.clear();
    m_body.clear();
    m_keep_alive = true;
    m_chunked = false;
    m_expect_continue = false;
}

// ---------------- HttpResponse ----------------

HttpResponse::HttpResponse(std::string serverName)
    : m_server_name(std::move(serverName))
{
}

void HttpResponse::SendError(HttpStatus s, std::string_view msg)
{
    m_status = s;
    m_headers.setHeader("Content-Type", "text/plain; charset=utf-8");
    m_body = msg.empty()
                 ? std::to_string(static_cast<uint16_t>(s)) + " " +
                       std::string(HttpStatusToString(s))
                 : std::string(msg);
}

void HttpResponse::SendRedirect(std::string_view location)
{
    m_status = HttpStatus::kFOUND;    // 302
    m_headers.setHeader("Location", location);
    m_headers.setHeader("Content-Type", "text/html; charset=utf-8");
    m_body = "<html><body>Redirecting to <a href=\"" + std::string(location) + "\">" +
             std::string(location) + "</a></body></html>";
}

std::string HttpResponse::Dump(const HttpRequest& req) const
{
    const bool is_head = (req.getMethod() == HttpMethod::kHEAD);
    const bool omit_body = is_head || StatusOmitsBody(m_status);
    const bool keep_alive = req.isKeepAlive() && m_keep_alive;

    std::string out;
    out.reserve(128 + m_headers.size() * 32 + m_cookies.size() * 48 + m_body.size());

    // 状态行：HTTP 版本跟随请求（HTTP/1.0 请求回 HTTP/1.0）
    out += "HTTP/1.";
    out += static_cast<char>(req.getHttpMinor() != 0 ? '1' : '0');
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
    for (const auto& [name, value] : m_headers)
    {
        if (IsManagedHeader(name))
        {
            LOG_FMT_WARN(g_logger, "HttpResponse::Dump: user header '%s' is managed by framework, skipped",
                         name.c_str());
            continue;
        }
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }

    // Set-Cookie 每条独立行
    for (const Cookie& c : m_cookies)
    {
        out += "Set-Cookie: ";
        out += SerializeSetCookie(c);
        out += "\r\n";
    }

    out += "Content-Length: ";
    out += std::to_string(m_body.size());    // HEAD 也输出应有长度
    out += "\r\nConnection: ";
    out += keep_alive ? "keep-alive" : "close";
    out += "\r\n\r\n";

    if (!omit_body && !m_body.empty())
    {
        out += m_body;
    }
    return out;
}

} // namespace bus::http
