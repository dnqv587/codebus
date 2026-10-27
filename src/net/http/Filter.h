//***********************************************
// name      : Filter.h
// brief     : HTTP 过滤器链（javax.servlet.Filter/FilterChain 对应物）：
//             鉴权/日志/编码等横切关注点，Servlet 之前顺序执行
// attention : Filter 与 Servlet 同线程执行（IO 线程或 worker 池）；
//             FilterChain 由 ServletDispatcher 构造，调用期间 Dispatcher
//             持 shared_ptr 保证裸指针生命周期
//***********************************************
#pragma once
#include <memory>
#include <span>

#include "net/http/HttpMessage.h"

namespace bus::http
{

class Servlet;
class FilterChain;

/// @brief 过滤器抽象（javax.servlet.Filter 对应物）
class Filter
{
 public:
    using ptr = std::shared_ptr<Filter>;
    virtual ~Filter() = default;

    /// @brief 过滤处理：调用 chain.doFilter(req, resp) 放行到下一环（或末端 Servlet）；
    ///        不调用即短路返回（如鉴权失败直接 resp.SendError(403)）
    /// @param req 完整请求（只读）
    /// @param resp 响应对象
    /// @param chain 过滤器链（放行用）
    virtual void doFilter(const HttpRequest& req, HttpResponse& resp, FilterChain& chain) = 0;
};

/// @brief 过滤器链（仅 ServletDispatcher 构造；逐环推进，链尽调目标 Servlet）
class FilterChain
{
 public:
    /// @brief 放行到下一环（最后一环之后调目标 Servlet）
    void doFilter(const HttpRequest& req, HttpResponse& resp);

 private:
    friend class ServletDispatcher;
    FilterChain(std::span<Filter* const> filters, Servlet* target);

    /// @brief 过滤器裸指针视图（Dispatcher 持 shared_ptr 保生命周期）
    std::span<Filter* const> m_filters;
    /// @brief 链尽后的目标 Servlet
    Servlet* m_target;
    /// @brief 当前推进下标
    size_t m_index = 0;
};

} // namespace bus::http
