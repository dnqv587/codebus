//***********************************************
// name      : ServletDispatcher.h
// brief     : 路由分发器（DispatcherServlet/HandlerMapping 对应物）：
//             精确/最长前缀路由 + 全局 Filter 链驱动 + 404/500 兜底；
//             本身也是 Servlet，可挂到另一 Dispatcher 下实现子路由嵌套
// attention : 路由与过滤器注册仅允许 Start 前调用（热路径零锁）
//***********************************************
#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "noncopyable.hpp"
#include "net/http/Filter.h"
#include "net/http/Servlet.h"

namespace bus::http {

class ServletDispatcher : public Servlet, noncopyable {
 public:
    using ptr = std::shared_ptr<ServletDispatcher>;

    /// @brief 精确路由注册（同路径后注册者胜）
    void addExactServlet(std::string_view path, Servlet::ptr s);

    /// @brief 前缀路由注册（最长者胜；段边界判定：/api 不匹配 /apiv2；同长后注册者胜）
    void addPrefixServlet(std::string_view prefix, Servlet::ptr s);

    /// @brief 追加全局过滤器（链尾；须 Start 前调用）
    void addFilter(Filter::ptr f);

    /// @brief 路由查询：精确 → 最长前缀 → nullptr（调用方走 NotFound 兜底）
    ATTR_NODISCARD
    Servlet::ptr getServlet(const HttpRequest& req) const;

    /// @brief 分发：Filter 链 → 目标 Servlet；Servlet/Filter 异常兜底 SendError(500)
    void Handle(const HttpRequest& req, HttpResponse& resp) override;

    ATTR_NODISCARD
    std::string_view getName() const noexcept override { return "ServletDispatcher"; }

 private:
    /// @brief 精确路由表
    std::unordered_map<std::string, Servlet::ptr> m_exact;
    /// @brief 前缀路由表（条目少，线性最长匹配）
    std::vector<std::pair<std::string, Servlet::ptr>> m_prefix;
    /// @brief 全局过滤器链
    std::vector<Filter::ptr> m_filters;
    /// @brief 404 兜底（值语义，免空判）
    NotFoundServlet m_default;
};

}  // namespace bus::http
