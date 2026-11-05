//***********************************************
// name      : ServletDispatcher.cc
// brief     : 路由分发器实现
// attention : None
//***********************************************
#include "net/http/ServletDispatcher.h"

#include <algorithm>

#include "log/Logger.h"
#include "net/http/HttpUtil.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");

namespace {

/// @brief 切路径段（"/a/b" → ["a","b"]；空与 "/" → 空向量）
inline std::vector<std::string_view> SplitSegments(std::string_view path) {
    std::vector<std::string_view> segs;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        std::string_view seg = path.substr(start, end - start);
        if (!seg.empty()) {
            segs.push_back(seg);
        }
        if (end == path.size()) {
            break;
        }
        start = end + 1;
    }
    return segs;
}

/// @brief 前缀匹配（段边界：/api 匹配 /api、/api/v2，不匹配 /apiv2）
inline bool prefixMatches(std::string_view path, std::string_view prefix) {
    if (!path.starts_with(prefix)) {
        return false;
    }
    return prefix == "/" || path.size() == prefix.size() || path[prefix.size()] == '/';
}

}  // namespace

void ServletDispatcher::addExactServlet(std::string_view path, Servlet::ptr s) {
    if (m_frozen.load(std::memory_order_acquire)) {
        LOG_WARN(g_logger) << "ServletDispatcher: frozen, addExactServlet('" << path << "') ignored";
        return;
    }
    m_exact[std::string(path)] = std::move(s);
}

void ServletDispatcher::addPrefixServlet(std::string_view prefix, Servlet::ptr s) {
    if (m_frozen.load(std::memory_order_acquire)) {
        LOG_WARN(g_logger) << "ServletDispatcher: frozen, addPrefixServlet('" << prefix << "') ignored";
        return;
    }
    std::string p(prefix);
    auto it = std::find_if(m_prefix.begin(), m_prefix.end(), [&p](const auto& kv) { return kv.first == p; });
    if (it != m_prefix.end()) {
        it->second = std::move(s);  // 同长前缀：后注册者胜
    } else {
        m_prefix.emplace_back(std::move(p), std::move(s));
    }
}

void ServletDispatcher::addFilter(Filter::ptr f) {
    if (m_frozen.load(std::memory_order_acquire)) {
        LOG_WARN(g_logger) << "ServletDispatcher: frozen, addFilter ignored";
        return;
    }
    m_filters.push_back(std::move(f));
}

Servlet::ptr ServletDispatcher::getServlet(const HttpRequest& req) const {
    auto it = m_exact.find(req.getPath());
    if (it != m_exact.end()) {
        return it->second;
    }
    // 模式层：全部命中中取静态段多者（同数后注册胜——线性 >= 保序）
    if (!m_patterns.empty()) {
        std::vector<std::string_view> segs = SplitSegments(req.getPath());
        Servlet::ptr best;
        const PatternMatcher* best_m = nullptr;
        std::vector<std::string_view> best_params;
        for (const auto& [matcher, s] : m_patterns) {
            std::vector<std::string_view> params;
            if (matcher.match(segs, &params) &&
                (best_m == nullptr || matcher.staticWeight >= best_m->staticWeight)) {
                best = s;
                best_m = &matcher;
                best_params = std::move(params);
            }
        }
        if (best != nullptr) {
            // 参数装配（mutable 装配区约定）：按段序与模式中参数段名配对
            size_t pi = 0;
            for (const std::string& seg : best_m->segments) {
                if (!seg.empty() && seg.front() == '{' && pi < best_params.size()) {
                    req.setPathParam(std::string_view(seg).substr(1, seg.size() - 2), best_params[pi++]);
                }
            }
            return best;
        }
    }
    // 最长前缀（>= 保证同长后注册者胜）
    Servlet::ptr best;
    size_t best_len = 0;
    for (const auto& [prefix, s] : m_prefix) {
        if (prefix.size() >= best_len && prefixMatches(req.getPath(), prefix)) {
            best_len = prefix.size();
            best = s;
        }
    }
    return best;
}

void ServletDispatcher::Handle(const HttpRequest& req, HttpResponse& resp) {
    try {
        // URL dot-segment 规范化（路由安全前置；越界 ".." → 400）——U2
        std::optional<std::string> normalized = NormalizePath(req.getPath());
        if (!normalized) {
            resp.SendError(HttpStatus::kBAD_REQUEST, "malformed path (dot-segment escape)");
            return;
        }
        if (*normalized != req.getPath()) {
            const_cast<HttpRequest&>(req).setPath(std::move(*normalized));  // 装配区约定
        }
        Servlet::ptr target = getServlet(req);
        Servlet* servlet = target ? target.get() : &m_default;
        // 过滤器链 = 全局链 + 命中模式链（注册序拼接；U2 per-path）
        std::vector<Filter*> chain;
        chain.reserve(m_filters.size() + m_pattern_filters.size());
        for (const auto& f : m_filters) {
            chain.push_back(f.get());
        }
        if (!m_pattern_filters.empty()) {
            std::vector<std::string_view> segs = SplitSegments(req.getPath());
            for (const auto& [matcher, f] : m_pattern_filters) {
                if (matcher.match(segs, nullptr)) {
                    chain.push_back(f.get());
                }
            }
        }
        if (chain.empty()) {
            servlet->Handle(req, resp);
        } else {
            FilterChain fc(chain, servlet);
            fc.doFilter(req, resp);  // 第一环启动
        }
    } catch (const std::exception& e) {
        // getPath() 返回 string_view（底层为 std::string 成员，data() 保证 NUL 终结）
        LOG_FMT_ERROR(g_logger, "ServletDispatcher: handler exception on %s: %s", req.getPath().data(), e.what());
        resp.SendError(HttpStatus::kINTERNAL_SERVER_ERROR, e.what());
    } catch (...) {
        LOG_FMT_ERROR(g_logger, "ServletDispatcher: unknown handler exception on %s", req.getPath().data());
        resp.SendError(HttpStatus::kINTERNAL_SERVER_ERROR, "internal error");
    }
}


// ---------------- PatternMatcher ----------------

bool ServletDispatcher::PatternMatcher::compile(std::string_view pattern, PatternMatcher* out) {
    out->segments.clear();
    out->staticWeight = 0;
    for (std::string_view seg : SplitSegments(pattern)) {
        if (!seg.empty() && seg.front() == '{') {
            if (seg.size() < 3 || seg.back() != '}') {
                return false;  // 非法参数段（空名/未闭合）
            }
        } else if (seg == "*") {
            if (!out->segments.empty()) {
                // 尾捕获：允许非首段 "*"（如 /static/*）；非法条件在尾部校验
            }
        }
        out->segments.push_back(std::string(seg));
        if (seg.front() != '{' && seg != "*") {
            ++out->staticWeight;
        }
    }
    // "*" 只允许末段
    for (size_t i = 0; i + 1 < out->segments.size(); ++i) {
        if (out->segments[i] == "*") {
            return false;
        }
    }
    return !out->segments.empty();
}

bool ServletDispatcher::PatternMatcher::match(const std::vector<std::string_view>& path_segs,
                                              std::vector<std::string_view>* params_out) const {
    const bool has_tail_star = (!segments.empty() && segments.back() == "*");
    const size_t fixed = has_tail_star ? segments.size() - 1 : segments.size();
    if (has_tail_star) {
        if (path_segs.size() < fixed) {
            return false;  // 前缀段不足
        }
    } else if (path_segs.size() != fixed) {
        return false;  // 段数严格相等
    }
    if (params_out != nullptr) {
        params_out->clear();
    }
    for (size_t i = 0; i < fixed; ++i) {
        const std::string& pat = segments[i];
        if (!pat.empty() && pat.front() == '{') {
            if (params_out != nullptr) {
                params_out->push_back(path_segs[i]);  // 参数捕获（名由调用方按序配对）
            }
        } else if (pat != path_segs[i]) {
            return false;
        }
    }
    return true;
}

// ---------------- 模式注册 ----------------

void ServletDispatcher::addPatternServlet(std::string_view pattern, Servlet::ptr s) {
    if (m_frozen.load(std::memory_order_acquire)) {
        LOG_WARN(g_logger) << "ServletDispatcher: frozen, addPatternServlet('" << pattern << "') ignored";
        return;
    }
    PatternMatcher m;
    if (!PatternMatcher::compile(pattern, &m)) {
        LOG_FMT_WARN(g_logger, "ServletDispatcher: invalid pattern '%.*s' ignored",
                     static_cast<int>(pattern.size()), pattern.data());
        return;
    }
    // 同模式重注册后者胜：先移除旧条目
    auto eq = [&m](const auto& kv) { return kv.first.segments == m.segments; };
    m_patterns.erase(std::remove_if(m_patterns.begin(), m_patterns.end(), eq), m_patterns.end());
    m_patterns.emplace_back(std::move(m), std::move(s));
}

void ServletDispatcher::addFilter(std::string_view pattern, Filter::ptr f) {
    if (m_frozen.load(std::memory_order_acquire)) {
        LOG_WARN(g_logger) << "ServletDispatcher: frozen, addFilter(pattern) ignored";
        return;
    }
    PatternMatcher m;
    if (!PatternMatcher::compile(pattern, &m)) {
        LOG_FMT_WARN(g_logger, "ServletDispatcher: invalid filter pattern '%.*s' ignored",
                     static_cast<int>(pattern.size()), pattern.data());
        return;
    }
    m_pattern_filters.emplace_back(std::move(m), std::move(f));
}

}  // namespace bus::http
