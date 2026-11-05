//***********************************************
// name      : ServletDispatcher.h
// brief     : 路由分发器（DispatcherServlet/HandlerMapping 对应物）：
//             精确/最长前缀路由 + 全局 Filter 链驱动 + 404/500 兜底；
//             本身也是 Servlet，可挂到另一 Dispatcher 下实现子路由嵌套
// attention : 路由与过滤器注册仅允许 Start 前调用（热路径零锁）
//***********************************************
#pragma once
#include <string>
#include <atomic>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "noncopyable.hpp"
#include "net/http/Filter.h"
#include "net/http/Servlet.h"

namespace bus::http {

/// @brief 透明字符串哈希（unordered_map 异构查找：string_view 键查询零拷贝）
struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

/// @brief 透明字符串相等（配合 StringHash 的异构查找）
struct StringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

class ServletDispatcher : public Servlet, noncopyable {
 public:
    using ptr = std::shared_ptr<ServletDispatcher>;

    /// @brief 精确路由注册（同路径后注册者胜）
    void addExactServlet(std::string_view path, Servlet::ptr s);

    /// @brief 前缀路由注册（最长者胜；段边界判定：/api 不匹配 /apiv2；同长后注册者胜）
    void addPrefixServlet(std::string_view prefix, Servlet::ptr s);

    /// @brief 模式路由注册（{name}=单段参数捕获；尾段 *=多段捕获；如 "/user/{id}"）
    /// @attention 非法模式（空参数名/非尾段 *）WARN+忽略；优先级：精确 > 模式
    ///            （静态段多者胜，同数后注册胜）> 前缀
    void addPatternServlet(std::string_view pattern, Servlet::ptr s);

    /// @brief 追加全局过滤器（链尾；须 Start 前调用）
    void addFilter(Filter::ptr f);

    /// @brief 路径过滤器注册（web.xml filter-mapping url-pattern 心智；模式语义同 addPatternServlet）
    /// @attention 执行序：全局链（注册序）→ 命中模式链（注册序）→ 目标 Servlet
    void addFilter(std::string_view pattern, Filter::ptr f);

    /// @brief 冻结注册（HttpServer::Start 自动调用；此后一切注册 WARN+忽略——T4 防护）
    void freeze() noexcept { m_frozen.store(true, std::memory_order_release); }

    /// @brief 路由查询：精确 → 最长前缀 → nullptr（调用方走 NotFound 兜底）
    ATTR_NODISCARD
    Servlet::ptr getServlet(const HttpRequest& req) const;

    /// @brief 分发：Filter 链 → 目标 Servlet；Servlet/Filter 异常兜底 SendError(500)
    void Handle(const HttpRequest& req, HttpResponse& resp) override;

    ATTR_NODISCARD
    std::string_view getName() const noexcept override {
        return "ServletDispatcher";
    }

 private:
    /// @brief 精确路由表（透明哈希：getPath() 的 string_view 直查零拷贝）
    std::unordered_map<std::string, Servlet::ptr, StringHash, StringEqual> m_exact;
    /// @brief 前缀路由表（条目少，线性最长匹配）
    std::vector<std::pair<std::string, Servlet::ptr>> m_prefix;
    /// @brief 全局过滤器链
    std::vector<Filter::ptr> m_filters;
    /// @brief 404 兜底（值语义，免空判）
    NotFoundServlet m_default;
    /// @brief 注册冻结标志（Start 后置位）
    std::atomic<bool> m_frozen{false};

    /// @brief 预编译模式段匹配器
    struct PatternMatcher {
        /// @brief 段序列（"{name}" 参数段原样存 / "*" 尾捕获 / 静态段原样）
        std::vector<std::string> segments;
        /// @brief 静态段数（模式间优先级权重）
        size_t staticWeight = 0;
        /// @brief 模式编译（非法模式返回 false）；参数名经 params_out_names 产出（按段序）
        static bool compile(std::string_view pattern, PatternMatcher* out);
        /// @brief 段匹配；命中且 params_out 非空时按序产出参数值
        bool match(const std::vector<std::string_view>& path_segs,
                   std::vector<std::string_view>* params_out) const;
    };
    /// @brief 模式路由表（注册序）
    std::vector<std::pair<PatternMatcher, Servlet::ptr>> m_patterns;
    /// @brief 模式过滤器表（注册序）
    std::vector<std::pair<PatternMatcher, Filter::ptr>> m_pattern_filters;
};

}  // namespace bus::http
