//***********************************************
// name      : HttpResponseParser.cc
// brief     : http_parser 响应模式适配器实现
// attention : None
//***********************************************
#include "net/http/HttpResponseParser.h"

#include "log/Logger.h"
#include "net/http/HttpUtil.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");

HttpResponseParser::HttpResponseParser(size_t maxBodyBytes)
    : m_max_body(maxBodyBytes) {
    http_parser_settings_init(&m_settings);
    m_settings.on_message_begin = &HttpResponseParser::onMessageBegin;
    m_settings.on_status = &HttpResponseParser::onStatus;
    m_settings.on_header_field = &HttpResponseParser::onHeaderField;
    m_settings.on_header_value = &HttpResponseParser::onHeaderValue;
    m_settings.on_headers_complete = &HttpResponseParser::onHeadersComplete;
    m_settings.on_body = &HttpResponseParser::onBody;
    m_settings.on_message_complete = &HttpResponseParser::onMessageComplete;
    http_parser_init(&m_parser, HTTP_RESPONSE);
    m_parser.data = this;
}

void HttpResponseParser::start(HttpMethod req_method) noexcept {
    // HEAD 响应无 body：C 库靠关联请求方法正确跳过（on_headers_complete 返回 1 语义由库内部处理）
    m_parser.method = static_cast<http_method>(req_method);
}

ParseResult HttpResponseParser::Execute(const char* data, size_t len) {
    ASSERT(len > 0, "Execute: len must be positive");
    m_error = HttpParseError::kNone;
    m_message_done = false;

    size_t nparsed = http_parser_execute(&m_parser, &m_settings, data, len);

    // 判定序同请求版：错误 → 完成 → errno → 需更多数据
    if (m_error != HttpParseError::kNone) {
        HttpParseError e = m_error;
        Reset();
        return {ParseEvent::kError, nparsed, e};
    }
    if (m_parser.upgrade) {
        return {ParseEvent::kUpgrade, nparsed, HttpParseError::kNone};
    }
    if (m_message_done) {
        ASSERT(nparsed > 0, "parser bug: complete with zero consumption");
        m_active = false;
        reinitForNext();
        return {ParseEvent::kComplete, nparsed, HttpParseError::kNone};
    }
    if (HTTP_PARSER_ERRNO(&m_parser) != HPE_OK) {
        HttpParseError e = (HTTP_PARSER_ERRNO(&m_parser) == HPE_HEADER_OVERFLOW)
                               ? HttpParseError::kHeaderTooLarge
                               : HttpParseError::kBadRequest;
        LOG_FMT_WARN(g_logger, "HttpResponseParser: parse error %s at byte %zu",
                     http_errno_name(HTTP_PARSER_ERRNO(&m_parser)), nparsed);
        Reset();
        return {ParseEvent::kError, nparsed, e};
    }
    return {ParseEvent::kNeedMore, nparsed, HttpParseError::kNone};
}

HttpResponse HttpResponseParser::takeResponse() {
    HttpResponse out = std::move(m_response);
    m_response = HttpResponse{};
    return out;
}

void HttpResponseParser::Reset() {
    reinitForNext();
    m_response = HttpResponse{};
    m_active = false;
}

// ---------------- C 回调 thunk ----------------

int HttpResponseParser::onMessageBegin(http_parser* p) {
    auto* self = static_cast<HttpResponseParser*>(p->data);
    self->m_active = true;
    self->m_response = HttpResponse{};
    return 0;
}

int HttpResponseParser::onStatus(http_parser* p, const char* at, size_t length) {
    (void)p;
    (void)at;
    (void)length;  // 原因短语由 HttpStatusToString 按状态码还原，不累积
    return 0;
}

int HttpResponseParser::onHeaderField(http_parser* p, const char* at, size_t length) {
    auto* self = static_cast<HttpResponseParser*>(p->data);
    if (self->m_last_was_value) {
        self->commitHeader();
        self->m_field.clear();
        self->m_value.clear();
        self->m_last_was_value = false;
    }
    self->m_field.append(at, length);
    return 0;
}

int HttpResponseParser::onHeaderValue(http_parser* p, const char* at, size_t length) {
    auto* self = static_cast<HttpResponseParser*>(p->data);
    self->m_value.append(at, length);
    self->m_last_was_value = true;
    return 0;
}

int HttpResponseParser::onHeadersComplete(http_parser* p) {
    auto* self = static_cast<HttpResponseParser*>(p->data);
    self->commitHeader();
    self->m_field.clear();
    self->m_value.clear();
    self->m_last_was_value = false;

    self->m_response.setStatus(static_cast<HttpStatus>(p->status_code));
    self->m_response.setKeepAlive(http_should_keep_alive(p) != 0);

    // CL 前置限额（零字节即拒）
    if (p->content_length != static_cast<uint64_t>(-1) && p->content_length > self->m_max_body) {
        return self->fail(HttpParseError::kBodyTooLarge);
    }
    // HEAD 响应无 body：返回 1 告知解析器（响应侧语义——请求侧恒 0）
    if (p->method == static_cast<unsigned char>(HTTP_HEAD)) {
        return 1;
    }
    return 0;
}

int HttpResponseParser::onBody(http_parser* p, const char* at, size_t length) {
    auto* self = static_cast<HttpResponseParser*>(p->data);
    if (self->m_response.getBody().size() + length > self->m_max_body) {
        return self->fail(HttpParseError::kBodyTooLarge);
    }
    self->m_response.appendBody(std::string_view(at, length));
    return 0;
}

int HttpResponseParser::onMessageComplete(http_parser* p) {
    auto* self = static_cast<HttpResponseParser*>(p->data);
    self->m_message_done = true;
    http_parser_pause(p, 1);  // 精确停在消息边界（keep-alive 下一响应续喂）
    return 0;
}

// ---------------- 私有辅助 ----------------

void HttpResponseParser::commitHeader() {
    if (!m_field.empty()) {
        m_response.setHeader(m_field, m_value);
    }
}

int HttpResponseParser::fail(HttpParseError e) {
    m_error = e;
    return 1;
}

void HttpResponseParser::reinitForNext() {
    http_parser_init(&m_parser, HTTP_RESPONSE);
    ASSERT(m_parser.data == this, "parser data pointer lost");
    m_field.clear();
    m_value.clear();
    m_last_was_value = false;
    m_message_done = false;
    m_error = HttpParseError::kNone;
}

}  // namespace bus::http
