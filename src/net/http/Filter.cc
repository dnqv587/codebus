//***********************************************
// name      : Filter.cc
// brief     : HTTP 过滤器链实现
// attention : None
//***********************************************
#include "net/http/Filter.h"

#include "net/http/Servlet.h"

namespace bus::http {

FilterChain::FilterChain(std::span<Filter* const> filters, Servlet* target) : m_filters(filters), m_target(target) {}

void FilterChain::doFilter(const HttpRequest& req, HttpResponse& resp) {
    if (m_index < m_filters.size()) {
        Filter* f = m_filters[m_index++];
        f->doFilter(req, resp, *this);
    } else {
        m_target->Handle(req, resp);
    }
}

}  // namespace bus::http
