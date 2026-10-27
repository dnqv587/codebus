//***********************************************
// name      : ServletDispatcher.cc
// brief     : 路由分发器实现
// attention : None
//***********************************************
#include "net/http/ServletDispatcher.h"

#include <algorithm>

#include "log/Logger.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");

namespace {

/// @brief 前缀匹配（段边界：/api 匹配 /api、/api/v2，不匹配 /apiv2）
inline bool prefixMatches(std::string_view path, std::string_view prefix) {
    if (!path.starts_with(prefix)) {
        return false;
    }
    return prefix == "/" || path.size() == prefix.size() || path[prefix.size()] == '/';
}

}  // namespace

void ServletDispatcher::addExactServlet(std::string_view path, Servlet::ptr s) {
    m_exact[std::string(path)] = std::move(s);
}

void ServletDispatcher::addPrefixServlet(std::string_view prefix, Servlet::ptr s) {
    std::string p(prefix);
    auto it = std::find_if(m_prefix.begin(), m_prefix.end(), [&p](const auto& kv) { return kv.first == p; });
    if (it != m_prefix.end()) {
        it->second = std::move(s);  // 同长前缀：后注册者胜
    } else {
        m_prefix.emplace_back(std::move(p), std::move(s));
    }
}

void ServletDispatcher::addFilter(Filter::ptr f) { m_filters.push_back(std::move(f)); }

Servlet::ptr ServletDispatcher::getServlet(const HttpRequest& req) const {
    auto it = m_exact.find(req.getPath());
    if (it != m_exact.end()) {
        return it->second;
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
        Servlet::ptr target = getServlet(req);
        Servlet* servlet = target ? target.get() : &m_default;
        if (m_filters.empty()) {
            servlet->Handle(req, resp);
        } else {
            std::vector<Filter*> chain;
            chain.reserve(m_filters.size());
            for (const auto& f : m_filters) {
                chain.push_back(f.get());
            }
            FilterChain fc(chain, servlet);
            fc.doFilter(req, resp);  // 第一环启动
        }
    } catch (const std::exception& e) {
        LOG_FMT_ERROR(g_logger, "ServletDispatcher: handler exception on %s: %s", req.getPath().c_str(), e.what());
        resp.SendError(HttpStatus::kINTERNAL_SERVER_ERROR, e.what());
    } catch (...) {
        LOG_FMT_ERROR(g_logger, "ServletDispatcher: unknown handler exception on %s", req.getPath().c_str());
        resp.SendError(HttpStatus::kINTERNAL_SERVER_ERROR, "internal error");
    }
}

}  // namespace bus::http
