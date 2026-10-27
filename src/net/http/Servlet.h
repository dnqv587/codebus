//***********************************************
// name      : Servlet.h
// brief     : 请求处理器抽象体系（javax.servlet 对应物）：Servlet 抽象基类、
//             HttpServlet 方法分派基类（doGet/doPost/...）、FunctionServlet
//             lambda 适配器、NotFoundServlet 404 兜底
// attention : doGet/doPost 等保持 Java 驼峰原名（Java 使用者零学习成本裁决）；
//             实现须异常安全——异常由 ServletDispatcher 兜底转 500
//***********************************************
#pragma once
#include <functional>
#include <memory>
#include <string_view>

#include "net/http/HttpMessage.h"

namespace bus::http
{

/// @brief 请求处理器抽象（javax.servlet.Servlet 对应物）
class Servlet
{
 public:
    using ptr = std::shared_ptr<Servlet>;
    virtual ~Servlet() = default;

    /// @brief 处理一个完整请求（对应 javax 的 service 方法）
    /// @param req 完整请求（只读）
    /// @param resp 响应对象（填充状态/头/body 后由框架序列化）
    virtual void Handle(const HttpRequest& req, HttpResponse& resp) = 0;

    /// @brief 处理器名（日志用）
    ATTR_NODISCARD virtual std::string_view getName() const noexcept { return "Servlet"; }
};

/// @brief 方法分派基类（对齐 javax.servlet.http.HttpServlet：未覆盖的方法自动 405）
class HttpServlet : public Servlet
{
 public:
    void Handle(const HttpRequest& req, HttpResponse& resp) override;   // switch(method) 分派

 protected:
    virtual void doGet(const HttpRequest& req, HttpResponse& resp)
    {
        (void)req;
        MethodNotAllowed(resp, "GET");
    }
    virtual void doPost(const HttpRequest& req, HttpResponse& resp)
    {
        (void)req;
        MethodNotAllowed(resp, "POST");
    }
    virtual void doPut(const HttpRequest& req, HttpResponse& resp)
    {
        (void)req;
        MethodNotAllowed(resp, "PUT");
    }
    virtual void doDelete(const HttpRequest& req, HttpResponse& resp)
    {
        (void)req;
        MethodNotAllowed(resp, "DELETE");
    }
    virtual void doPatch(const HttpRequest& req, HttpResponse& resp)
    {
        (void)req;
        MethodNotAllowed(resp, "PATCH");
    }
    /// @brief 默认转调 doGet（Dump 依 HEAD 规则剥 body 字节，安全复用；Java 容器同语义）
    virtual void doHead(const HttpRequest& req, HttpResponse& resp)
    {
        doGet(req, resp);
    }
    /// @brief 回 Allow 清单（javax 默认语义）
    virtual void doOptions(const HttpRequest& req, HttpResponse& resp);

 private:
    /// @brief 405 + Allow 头（Allow 值为触发方法：基类无从得知子类覆盖集）
    static void MethodNotAllowed(HttpResponse& resp, std::string_view method);
};

/// @brief std::function 适配器（轻量路由 handler 用）
class FunctionServlet : public Servlet
{
 public:
    using Handler = std::function<void (const HttpRequest&, HttpResponse&)>;

    explicit FunctionServlet(Handler h);
    void Handle(const HttpRequest& req, HttpResponse& resp) override;
    ATTR_NODISCARD std::string_view getName() const noexcept override { return "Function"; }

 private:
    /// @brief 业务处理器
    Handler m_handler;
};

/// @brief 兜底 404（Dispatcher 未命中时的默认目标）
class NotFoundServlet : public Servlet
{
 public:
    void Handle(const HttpRequest& req, HttpResponse& resp) override;
    ATTR_NODISCARD std::string_view getName() const noexcept override { return "NotFound"; }
};

} // namespace bus::http
