//***********************************************
// name      : Servlet.cc
// brief     : 请求处理器体系实现
// attention : None
//***********************************************
#include "net/http/Servlet.h"

namespace bus::http
{

void HttpServlet::Handle(const HttpRequest& req, HttpResponse& resp)
{
    switch (req.getMethod())
    {
        case HttpMethod::kGET:
            doGet(req, resp);
            break;
        case HttpMethod::kPOST:
            doPost(req, resp);
            break;
        case HttpMethod::kPUT:
            doPut(req, resp);
            break;
        case HttpMethod::kDELETE:
            doDelete(req, resp);
            break;
        case HttpMethod::kPATCH:
            doPatch(req, resp);
            break;
        case HttpMethod::kHEAD:
            doHead(req, resp);
            break;
        case HttpMethod::kOPTIONS:
            doOptions(req, resp);
            break;
        default:
            MethodNotAllowed(resp, HttpMethodToString(req.getMethod()));
            break;
    }
}

void HttpServlet::doOptions(const HttpRequest&, HttpResponse& resp)
{
    resp.setHeader("Allow", "GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");
    resp.setStatus(HttpStatus::kOK);
    resp.setBody(std::string_view(""));
}

void HttpServlet::MethodNotAllowed(HttpResponse& resp, std::string_view method)
{
    resp.setHeader("Allow", method);
    resp.SendError(HttpStatus::kMETHOD_NOT_ALLOWED);
}

FunctionServlet::FunctionServlet(Handler h)
    : m_handler(std::move(h))
{
}

void FunctionServlet::Handle(const HttpRequest& req, HttpResponse& resp)
{
    m_handler(req, resp);
}

void NotFoundServlet::Handle(const HttpRequest& req, HttpResponse& resp)
{
    resp.SendError(HttpStatus::kNOT_FOUND, "not found: " + std::string(req.getPath()));
}

} // namespace bus::http
