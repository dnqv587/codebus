//***********************************************
// name      : HttpRequestParser.cc
// brief     : 3rd/http_parser 请求解析适配器实现
// attention : None
//***********************************************
#include "net/http/HttpRequestParser.h"

#include "log/Logger.h"
#include "net/http/HttpUtil.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");

HttpRequestParser::HttpRequestParser(size_t maxBodyBytes, size_t maxUrlBytes)
    : m_max_body(maxBodyBytes),
      m_max_url(maxUrlBytes) {
    http_parser_settings_init(&m_settings);
    m_settings.on_message_begin = &HttpRequestParser::onMessageBegin;
    m_settings.on_url = &HttpRequestParser::onUrl;
    m_settings.on_header_field = &HttpRequestParser::onHeaderField;
    m_settings.on_header_value = &HttpRequestParser::onHeaderValue;
    m_settings.on_headers_complete = &HttpRequestParser::onHeadersComplete;
    m_settings.on_body = &HttpRequestParser::onBody;
    m_settings.on_message_complete = &HttpRequestParser::onMessageComplete;
    http_parser_init(&m_parser, HTTP_REQUEST);
    m_parser.data = this;
}

ParseResult HttpRequestParser::Execute(const char* data, size_t len) {
    ASSERT(len > 0, "Execute: len must be positive");
    m_error = HttpParseError::kNone;
    m_message_done = false;

    size_t nparsed = http_parser_execute(&m_parser, &m_settings, data, len);

    // 判定序：错误 → 升级 → 完成 → errno → 需更多数据。
    // 错误必须优先：CL 超限 fail() 返回 1 时解析器按"无 body"语义直达
    // on_message_complete，若先判完成会把 413 误报成 kComplete。
    if (m_error != HttpParseError::kNone) {
        HttpParseError e = m_error;
        Reset();
        return {ParseEvent::kError, nparsed, e};
    }
    if (m_parser.upgrade) {
        return {ParseEvent::kUpgrade, nparsed, HttpParseError::kNone};
    }
    if (m_message_done) {
        // 消息完成：完成 query 解码、自复位（pipeline 续喂就绪）
        ASSERT(nparsed > 0, "parser bug: complete with zero consumption");
        m_request.finishParams();
        ++m_parsed_count;
        m_active = false;
        reinitForNext();
        return {ParseEvent::kComplete, nparsed, HttpParseError::kNone};
    }
    if (HTTP_PARSER_ERRNO(&m_parser) != HPE_OK) {
        // 协议层错误（非回调侧）
        HttpParseError e = (HTTP_PARSER_ERRNO(&m_parser) == HPE_HEADER_OVERFLOW) ? HttpParseError::kHeaderTooLarge
                                                                                 : HttpParseError::kBadRequest;
        LOG_FMT_WARN(g_logger, "HttpRequestParser: parse error %s at byte %zu",
                     http_errno_name(HTTP_PARSER_ERRNO(&m_parser)), nparsed);
        Reset();
        return {ParseEvent::kError, nparsed, e};
    }
    return {ParseEvent::kNeedMore, nparsed, HttpParseError::kNone};
}

HttpRequest HttpRequestParser::TakeRequest() {
    HttpRequest out;
    out.Swap(m_request);
    return out;
}

void HttpRequestParser::Reset() {
    reinitForNext();
    m_request.clear();
    m_active = false;
}

// ---------------- C 回调 thunk ----------------

int HttpRequestParser::onMessageBegin(http_parser* p) {
    auto* self = static_cast<HttpRequestParser*>(p->data);
    self->m_active = true;
    self->m_request.clear();  // 复位上一条遗留（未 Take 的 moved-from 态）
    if (self->m_on_message_begin) {
        self->m_on_message_begin();
    }
    return 0;
}

int HttpRequestParser::onUrl(http_parser* p, const char* at, size_t length) {
    auto* self = static_cast<HttpRequestParser*>(p->data);
    if (self->m_url.size() + length > self->m_max_url) {
        return self->fail(HttpParseError::kUrlTooLong);
    }
    self->m_url.append(at, length);
    self->m_request.appendUrl(std::string_view(at, length));  // 原始 URL 同步装配
    return 0;
}

int HttpRequestParser::onHeaderField(http_parser* p, const char* at, size_t length) {
    auto* self = static_cast<HttpRequestParser*>(p->data);
    if (self->m_last_was_value) {
        // 上一对已齐：先收割再开新对（C 库保证 field 群/value 群严格交替）
        self->commitHeader();
        self->m_field.clear();
        self->m_value.clear();
        self->m_last_was_value = false;
    }
    self->m_field.append(at, length);
    return 0;
}

int HttpRequestParser::onHeaderValue(http_parser* p, const char* at, size_t length) {
    auto* self = static_cast<HttpRequestParser*>(p->data);
    self->m_value.append(at, length);
    self->m_last_was_value = true;
    return 0;
}

int HttpRequestParser::onHeadersComplete(http_parser* p) {
    auto* self = static_cast<HttpRequestParser*>(p->data);
    self->commitHeader();  // 补收割末对 header

    // URL 切分：优先 http_parser_parse_url；失败回退手找 '?'
    // （绝对 URL/asterisk-form 等非常规形态容错）
    http_parser_url u;
    http_parser_url_init(&u);
    std::string path, query;
    if (http_parser_parse_url(self->m_url.data(), self->m_url.size(), 0, &u) == 0 && (u.field_set & (1u << UF_PATH))) {
        path.assign(self->m_url, u.field_data[UF_PATH].off, u.field_data[UF_PATH].len);
        if (u.field_set & (1u << UF_QUERY)) {
            query.assign(self->m_url, u.field_data[UF_QUERY].off, u.field_data[UF_QUERY].len);
        }
    } else {
        size_t q = self->m_url.find('?');
        if (q == std::string::npos) {
            path = self->m_url;
        } else {
            path = self->m_url.substr(0, q);
            query = self->m_url.substr(q + 1);
        }
    }

    // path 解码（%XX；'+' 字面量）；%00 命中即拒（防路径截断攻击）
    std::string decoded = UrlDecode(path, false);
    if (decoded.find('\0') != std::string::npos) {
        return self->fail(HttpParseError::kBadRequest);
    }
    self->m_request.setPath(std::move(decoded));
    self->m_request.setQuery(std::move(query));

    self->m_request.setMethod(static_cast<HttpMethod>(p->method));
    self->m_request.setMajor(p->http_major);
    self->m_request.setMinor(p->http_minor);
    self->m_request.setKeepAlive(http_should_keep_alive(p) != 0);
    self->m_request.setChunked((p->flags & F_CHUNKED) != 0);

    // Expect: 100-continue（不处理会导致 curl 大 POST hang）
    auto expect = self->m_request.getHeader("expect");
    if (expect && EqualCI(*expect, "100-continue")) {
        self->m_request.setExpectContinue(true);
        if (self->m_on_expect_continue) {
            self->m_on_expect_continue();
        }
    }

    // Cookie 头此时已齐，立即解析
    self->m_request.finishCookies();

    // CL 前置限额：零字节即拒（返回 1 = 无 body 语义直达消息界）
    if (p->content_length != static_cast<uint64_t>(-1) && p->content_length > self->m_max_body) {
        return self->fail(HttpParseError::kBodyTooLarge);
    }
    return 0;
}

int HttpRequestParser::onBody(http_parser* p, const char* at, size_t length) {
    auto* self = static_cast<HttpRequestParser*>(p->data);
    if (self->m_request.getBody().size() + length > self->m_max_body) {
        return self->fail(HttpParseError::kBodyTooLarge);  // 累计断（覆盖 chunked/谎报 CL）
    }
    self->m_request.appendBody(std::string_view(at, length));
    return 0;
}

int HttpRequestParser::onMessageComplete(http_parser* p) {
    auto* self = static_cast<HttpRequestParser*>(p->data);
    self->m_message_done = true;
    http_parser_pause(p, 1);  // 精确停在消息边界（否则吞掉 pipelined 下一条）
    return 0;
}

// ---------------- 私有辅助 ----------------

void HttpRequestParser::commitHeader() {
    if (!m_field.empty()) {
        m_request.addHeaderRaw(m_field, m_value);
    }
}

int HttpRequestParser::fail(HttpParseError e) {
    m_error = e;
    return 1;  // 返回非 0 → 解析器停机（HPE_CB_*）
}

void HttpRequestParser::reinitForNext() {
    // 无 http_parser_reinit：重调 init 即复用（data 指针被 init 保留）
    http_parser_init(&m_parser, HTTP_REQUEST);
    ASSERT(m_parser.data == this, "parser data pointer lost");
    m_url.clear();
    m_field.clear();
    m_value.clear();
    m_last_was_value = false;
    m_message_done = false;
    m_error = HttpParseError::kNone;
}

}  // namespace bus::http
