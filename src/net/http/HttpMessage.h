//***********************************************
// name      : HttpMessage.h
// brief     : HTTP 协议对象层：HeaderMap（大小写不敏感头集）、Cookie、
//             HttpRequest/HttpResponse（Java Servlet 接口对应物）与响应序列化
// attention : HttpRequest 装配器（setXxx/appendXxx）仅供 parser 与测试使用；
//             HttpResponse::Dump 的托管头（Content-Length/Connection/Server/
//             Set-Cookie/Transfer-Encoding）由框架输出，用户设置将被跳过并 WARN
//***********************************************
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Macro.h"
#include "copyable.hpp"
#include "net/http/HttpProtocol.h"

namespace bus::http {

/// @brief HTTP 头集：原序 vector + ASCII 大小写不敏感查找（典型 n≤40，缓存友好天然保序）
class HeaderMap : public copyable {
 public:
    /// @brief 取首个命中头值
    /// @param name 头名（大小写不敏感）
    /// @return 值；不存在返回 nullopt
    ATTR_NODISCARD
    std::optional<std::string_view> getHeader(std::string_view name) const noexcept;
    /// @brief 取全部同名头值（Cookie/Set-Cookie 场景）
    ATTR_NODISCARD
    std::vector<std::string_view> getHeaders(std::string_view name) const;
    /// @brief 覆盖同名（删旧追加新，保尾序）
    void setHeader(std::string_view name, std::string_view value);
    /// @brief 追加一条（允许重复头）
    void addHeader(std::string_view name, std::string_view value);
    ATTR_NODISCARD
    bool hasHeader(std::string_view name) const noexcept;
    ATTR_PURE_INLINE
    size_t size() const noexcept {
        return m_headers.size();
    }
    /// @brief 只读迭代（序列化/日志用）
    ATTR_NODISCARD
    auto begin() const noexcept {
        return m_headers.begin();
    }
    ATTR_NODISCARD
    auto end() const noexcept {
        return m_headers.end();
    }
    void clear() noexcept;
    /// @brief 底层数组交换（零拷贝）
    void Swap(HeaderMap& o) noexcept;

 private:
    /// @brief 头对（原插入序）
    std::vector<std::pair<std::string, std::string>> m_headers;
};

/// @brief Cookie（RFC 6265）：请求侧仅填 name/value；响应侧属性空值省略输出
struct Cookie : public copyable {
    std::string name;
    std::string value;
    std::string path = "/";  ///< 空 = 不输出 Path 属性
    std::string domain;      ///< 空 = 不输出
    std::string expires;     ///< RFC1123 串原样输出；空 = 不输出
    int64_t maxAge = -1;     ///< <0 = 不输出 Max-Age
    bool secure = false;
    bool httpOnly = false;
    std::string sameSite;  ///< "Lax"/"Strict"/"None"；空 = 不输出
};

/// @brief 解析请求 Cookie 头（仅名值对；';' 切分、trim、无 '=' 段跳过）
ATTR_NODISCARD
std::vector<Cookie> ParseRequestCookies(std::string_view header) noexcept;

/// @brief 序列化 Set-Cookie 行值（不含头名与 CRLF；name/value 由调用方保证安全字符）
ATTR_NODISCARD
std::string SerializeSetCookie(const Cookie& c) noexcept;

/// @brief HTTP 请求（HttpServletRequest 对应物；parser 装配、业务只读）
class HttpRequest : public copyable {
 public:
    // ---- 请求行 ----
    ATTR_PURE_INLINE
    HttpMethod getMethod() const noexcept {
        return m_method;
    }
    ATTR_PURE_INLINE
    uint16_t getHttpMajor() const noexcept {
        return m_major;
    }
    ATTR_PURE_INLINE
    uint16_t getHttpMinor() const noexcept {
        return m_minor;
    }
    /// @return "HTTP/1.1" 形态
    ATTR_NODISCARD
    std::string getVersion() const noexcept;
    ATTR_NODISCARD
    const std::string& getRawUrl() const noexcept {
        return m_raw_url;
    }
    /// @return %XX 已解码、'+' 未解码的路径
    ATTR_NODISCARD
    const std::string& getPath() const noexcept {
        return m_path;
    }
    /// @return 原始 query 串（不含 '?'）
    ATTR_NODISCARD
    const std::string& getQuery() const noexcept {
        return m_query;
    }
    // ---- 参数/头/Cookie（query 已预解码，'+' 按空格）----
    ATTR_NODISCARD
    std::optional<std::string_view> getParam(std::string_view key) const;
    ATTR_NODISCARD
    std::vector<std::string_view> getParams(std::string_view key) const;
    ATTR_NODISCARD
    std::optional<std::string_view> getHeader(std::string_view name) const noexcept {
        return m_headers.getHeader(name);
    }
    /// @brief 取全部同名头值（Java HttpServletRequest.getHeaders 对应物）
    ATTR_NODISCARD
    std::vector<std::string_view> getHeaders(std::string_view name) const {
        return m_headers.getHeaders(name);
    }
    ATTR_NODISCARD
    std::span<const Cookie> getCookies() const noexcept {
        return m_cookies;
    }
    ATTR_NODISCARD
    std::optional<std::string_view> getCookie(std::string_view name) const;
    // ---- body / 连接语义 ----
    ATTR_NODISCARD
    std::string_view getBody() const noexcept {
        return m_body;
    }
    ATTR_PURE_INLINE
    size_t getContentLength() const noexcept {
        return m_body.size();
    }
    ATTR_NODISCARD
    bool isKeepAlive() const noexcept {
        return m_keep_alive;
    }
    ATTR_NODISCARD
    bool isChunked() const noexcept {
        return m_chunked;
    }
    ATTR_NODISCARD
    bool isExpectContinue() const noexcept {
        return m_expect_continue;
    }
    /// @brief 零拷贝交接（parser 复用请求对象）
    void Swap(HttpRequest& o) noexcept;

    // ---- 装配器（public 但仅供 HttpRequestParser 与测试；业务侧勿调）----
    void setMethod(HttpMethod m) noexcept {
        m_method = m;
    }
    void setMajor(uint16_t v) noexcept {
        m_major = v;
    }
    void setMinor(uint16_t v) noexcept {
        m_minor = v;
    }
    void appendUrl(std::string_view s) {
        m_raw_url.append(s);
    }
    void setPath(std::string s) {
        m_path = std::move(s);
    }
    void setQuery(std::string s) {
        m_query = std::move(s);
    }
    void addHeaderRaw(std::string_view name, std::string_view value) {
        m_headers.addHeader(name, value);
    }
    void appendBody(std::string_view s) {
        m_body.append(s);
    }
    void setKeepAlive(bool on) noexcept {
        m_keep_alive = on;
    }
    void setChunked(bool on) noexcept {
        m_chunked = on;
    }
    void setExpectContinue(bool on) noexcept {
        m_expect_continue = on;
    }
    /// @brief 由原始 query 串解码参数（parser 于消息完成时调用）
    void finishParams();
    /// @brief 由 Cookie 头解析名值对（parser 于头完成时调用）
    void finishCookies();
    /// @brief 复位为默认状态（parser 复用）
    void clear() noexcept;

 private:
    /// @brief 方法
    HttpMethod m_method = HttpMethod::kGET;
    /// @brief 主版本号
    uint16_t m_major = 1;
    /// @brief 次版本号
    uint16_t m_minor = 1;
    /// @brief 原始 URL（未解码）
    std::string m_raw_url;
    /// @brief 已解码路径（'+' 保留字面）
    std::string m_path;
    /// @brief 原始 query 串
    std::string m_query;
    /// @brief 头集
    HeaderMap m_headers;
    /// @brief 已解码 query 参数对
    std::vector<std::pair<std::string, std::string>> m_params;
    /// @brief 请求 Cookie
    std::vector<Cookie> m_cookies;
    /// @brief 请求体
    std::string m_body;
    /// @brief 连接保持（http_should_keep_alive 结果）
    bool m_keep_alive = true;
    /// @brief chunked 传输编码
    bool m_chunked = false;
    /// @brief Expect: 100-continue
    bool m_expect_continue = false;
};

/// @brief HTTP 响应（HttpServletResponse 对应物）
class HttpResponse : public copyable {
 public:
    explicit HttpResponse(std::string serverName = "codebus-http");
    ATTR_PURE_INLINE
    HttpStatus getStatus() const noexcept {
        return m_status;
    }
    void setStatus(HttpStatus s) noexcept {
        m_status = s;
    }
    void setHeader(std::string_view name, std::string_view value) {
        m_headers.setHeader(name, value);
    }
    /// @brief 读回已设头（Java HttpServletResponse.getHeader 对应物）
    ATTR_NODISCARD
    std::optional<std::string_view> getHeader(std::string_view name) const noexcept {
        return m_headers.getHeader(name);
    }
    /// @brief 取全部同名头值（Java HttpServletRequest.getHeaders 对应物）
    ATTR_NODISCARD
    std::vector<std::string_view> getHeaders(std::string_view name) const {
        return m_headers.getHeaders(name);
    }
    void addHeader(std::string_view name, std::string_view value) {
        m_headers.addHeader(name, value);
    }
    /// @brief 添加 Set-Cookie（Java HttpServletResponse.addCookie）
    void addCookie(Cookie c) {
        m_cookies.push_back(std::move(c));
    }
    void setBody(std::string body) noexcept {
        m_body = std::move(body);
    }
    void setBody(std::string_view body) {
        m_body.assign(body);
    }
    void appendBody(std::string_view chunk) {
        m_body.append(chunk);
    }
    ATTR_NODISCARD
    std::string_view getBody() const noexcept {
        return m_body;
    }
    /// @brief 错误页：状态码 + text/plain 简短 body（msg 空则 "<code> <reason>"）
    void SendError(HttpStatus s, std::string_view msg = {});
    /// @brief 302 重定向 + Location + 短 html body
    void SendRedirect(std::string_view location);
    void setKeepAlive(bool on) noexcept {
        m_keep_alive = on;
    }
    ATTR_NODISCARD
    bool isKeepAlive() const noexcept {
        return m_keep_alive;
    }
    /// @brief 序列化（不修改本对象）：版本/keep-alive 跟随请求；HEAD/1xx/204/304 剥 body
    /// @param req 关联请求（提供版本、方法、连接语义）
    ATTR_NODISCARD
    std::string Dump(const HttpRequest& req) const;

 private:
    /// @brief 状态码
    HttpStatus m_status = HttpStatus::kOK;
    /// @brief Server 头值
    std::string m_server_name;
    /// @brief 用户头集
    HeaderMap m_headers;
    /// @brief 待输出 Cookie
    std::vector<Cookie> m_cookies;
    /// @brief 响应体
    std::string m_body;
    /// @brief 连接保持（默认跟随请求）
    bool m_keep_alive = true;
};

}  // namespace bus::http
