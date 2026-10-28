//***********************************************
// name      : HttpResponseParser.h
// brief     : 3rd/http_parser 响应模式适配器（复用请求版封装模式：分片累加/
//             限额/错误翻译/pause 精确边界；请求/响应装配差异大不共享代码）
// attention : HEAD 响应必须先 start(kHEAD)——C 库靠 parser.method 跳 body；
//             kError 后 parser 已复位勿 takeResponse
//***********************************************
#pragma once
#include <cstddef>
#include <string>

#include "noncopyable.hpp"
#include "type.h"
#include "http_parser.h"
#include "net/http/HttpMessage.h"
#include "net/http/HttpRequestParser.h"  // ParseEvent/ParseResult/HttpParseError 复用

namespace bus::http {

/// @brief HTTP 响应解析器（HTTP_RESPONSE 模式）
class HttpResponseParser : noncopyable {
 public:
    explicit HttpResponseParser(size_t maxBodyBytes = 8_MB);
    ~HttpResponseParser() = default;

    /// @brief 关联请求方法（HEAD 响应必须先调——C 库据此跳过 body）
    void start(HttpMethod req_method) noexcept;

    /// @brief 增量喂入；kComplete 后自动复位可续喂（keep-alive 下一响应）
    /// @param data 数据指针
    /// @param len 字节数（必须 > 0）
    ParseResult Execute(const char* data, size_t len);

    /// @brief 取走已完整的响应（kComplete 后有效；move 出）
    ATTR_NODISCARD
    HttpResponse takeResponse();

    /// @brief 显式复位（等同重新构造）
    void Reset();

 private:
    // ---- C 回调静态 thunk ----
    static int onMessageBegin(http_parser* p);
    static int onStatus(http_parser* p, const char* at, size_t length);
    static int onHeaderField(http_parser* p, const char* at, size_t length);
    static int onHeaderValue(http_parser* p, const char* at, size_t length);
    static int onHeadersComplete(http_parser* p);
    static int onBody(http_parser* p, const char* at, size_t length);
    static int onMessageComplete(http_parser* p);

    void commitHeader();
    int fail(HttpParseError e);
    void reinitForNext();

    /// @brief C 解析器实例
    http_parser m_parser;
    /// @brief 回调表
    http_parser_settings m_settings;
    /// @brief 正在装配的响应
    HttpResponse m_response;
    /// @brief 当前 header 字段名累加器
    std::string m_field;
    /// @brief 当前 header 值累加器
    std::string m_value;
    /// @brief 上一 header 回调是否为 value
    bool m_last_was_value = false;
    /// @brief 消息完成标志
    bool m_message_done = false;
    /// @brief 有进行中消息
    bool m_active = false;
    /// @brief 回调侧错误
    HttpParseError m_error = HttpParseError::kNone;
    /// @brief body 上限
    size_t m_max_body;
};

}  // namespace bus::http
