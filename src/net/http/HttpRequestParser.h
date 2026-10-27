//***********************************************
// name      : HttpRequestParser.h
// brief     : 3rd/http_parser 的请求解析适配器：增量喂字节、分片回调拼接、
//             限额检查（body/URL/头）、错误翻译与 pipeline 续喂
// attention : C API 仅泄漏到本文件（未来换 llhttp 只动此处）；
//             kError 后 parser 已整体复位勿 TakeRequest；kUpgrade 后保留
//             请求头信息（transport 需回 501），弃用前可 Reset
//***********************************************
#pragma once
#include <cstddef>
#include <functional>
#include <string>

#include "Macro.h"
#include "noncopyable.hpp"
#include "type.h"
#include "http_parser.h"
#include "net/http/HttpMessage.h"

namespace bus::http {

enum class ParseEvent {
    kNeedMore,  ///< 数据不足，继续喂
    kComplete,  ///< 一条请求完成（TakeRequest 取走；parser 已复位可续喂）
    kError,     ///< 解析错误（见 HttpParseError；parser 已复位）
    kUpgrade    ///< 升级协议（WebSocket/CONNECT；consumed 指向头部结束处）
};

enum class HttpParseError {
    kNone,
    kBadRequest,      ///< 400：协议错误总类（坏版本/方法/URL/chunk/重复 CL 等）
    kHeaderTooLarge,  ///< 431：头超 HTTP_MAX_HEADER_SIZE（80KB 内建）
    kBodyTooLarge,    ///< 413：CL 前置超限或累计超限
    kUrlTooLong       ///< 414：URL 超限
};

struct ParseResult {
    ParseEvent event = ParseEvent::kNeedMore;
    /// @brief 本次消费的字节数（kComplete/kUpgrade 边界精确；调用方据此 SkipRead）
    size_t consumed = 0;
    HttpParseError error = HttpParseError::kNone;
};

class HttpRequestParser : noncopyable {
 public:
    /// @brief 头完成且含 Expect: 100-continue 时触发（transport 立即回 100）
    using ExpectContinueCallback = std::function<void()>;
    /// @brief 新请求首字节触发（慢速攻击计时用）
    using MessageBeginCallback = std::function<void()>;

    /// @param maxBodyBytes 请求体上限（CL 前置 + 累计双保险）
    /// @param maxUrlBytes URL 上限
    explicit HttpRequestParser(size_t maxBodyBytes = 8_MB, size_t maxUrlBytes = 8_KB);
    ~HttpRequestParser() = default;

    /// @brief 增量喂入一段字节；kComplete 后自动复位，可继续喂（pipeline）
    /// @param data 数据指针
    /// @param len 字节数（必须 > 0）
    /// @return 判定序：错误 → 升级 → 完成 → 需更多数据（错误优先：CL 超限的
    ///         fail() 返回 1 会让解析器按"无 body"语义直达 message_complete）
    ParseResult Execute(const char* data, size_t len);

    /// @brief 取走已完成的请求（kComplete 后有效；move 出）
    ATTR_NODISCARD
    HttpRequest TakeRequest();

    /// @brief 显式复位（等同重新构造；Execute 在 kComplete/kError 后已自动复位）
    void Reset();

    /// @brief 是否空闲（无进行中的请求）
    ATTR_NODISCARD
    bool isIdle() const noexcept { return !m_active; }
    /// @brief 已完成请求数（会话统计）
    ATTR_PURE_INLINE
    size_t parsedCount() const noexcept { return m_parsed_count; }

    void setOnExpectContinue(ExpectContinueCallback cb) noexcept { m_on_expect_continue = std::move(cb); }
    void setOnMessageBegin(MessageBeginCallback cb) noexcept { m_on_message_begin = std::move(cb); }

 private:
    // ---- C 回调静态 thunk（parser.data 指向本对象）----
    static int onMessageBegin(http_parser* p);
    static int onUrl(http_parser* p, const char* at, size_t length);
    static int onHeaderField(http_parser* p, const char* at, size_t length);
    static int onHeaderValue(http_parser* p, const char* at, size_t length);
    static int onHeadersComplete(http_parser* p);
    static int onBody(http_parser* p, const char* at, size_t length);
    static int onMessageComplete(http_parser* p);

    /// @brief 收割当前 header 配对入请求
    void commitHeader();
    /// @brief 错误置位（回调内；返回 1 使解析器停机）
    int fail(HttpParseError e);
    /// @brief 为下一条消息复位解析器与累加器（请求对象保留待 TakeRequest）
    void reinitForNext();

    /// @brief C 解析器实例
    http_parser m_parser;
    /// @brief 回调表（静态 thunk）
    http_parser_settings m_settings;
    /// @brief 正在装配的请求
    HttpRequest m_request;
    /// @brief URL 累加器（on_url 分片拼接）
    std::string m_url;
    /// @brief 当前 header 字段名累加器
    std::string m_field;
    /// @brief 当前 header 值累加器
    std::string m_value;
    /// @brief 上一 header 回调是否为 value（配对状态机）
    bool m_last_was_value = false;
    /// @brief body 上限
    size_t m_max_body;
    /// @brief URL 上限
    size_t m_max_url;
    /// @brief 新请求首字节回调
    MessageBeginCallback m_on_message_begin;
    /// @brief Expect: 100-continue 回调
    ExpectContinueCallback m_on_expect_continue;
    /// @brief 回调侧错误（优先于 errno 翻译）
    HttpParseError m_error = HttpParseError::kNone;
    /// @brief 消息完成标志
    bool m_message_done = false;
    /// @brief 有进行中请求
    bool m_active = false;
    /// @brief 已完成请求数
    size_t m_parsed_count = 0;
};

}  // namespace bus::http
