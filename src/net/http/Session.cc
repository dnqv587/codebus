//***********************************************
// name      : Session.cc
// brief     : HTTP 会话管理实现
// attention : None
//***********************************************
#include "net/http/Session.h"

#include <chrono>
#include <random>

namespace bus::http {

namespace {

/// @brief steady 时钟纳秒
inline int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ---------------- Session ----------------

Session::Session(std::string id)
    : m_id(std::move(id)),
      m_last_access_ns(nowNs()) {
}

void Session::setAttribute(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_attrs.insert_or_assign(std::string(key), std::string(value));
}

std::optional<std::string_view> Session::getAttribute(std::string_view key) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_attrs.find(std::string(key));
    if (it == m_attrs.end()) {
        return std::nullopt;
    }
    return std::string_view(it->second);
}

void Session::eraseAttribute(std::string_view key) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_attrs.erase(std::string(key));
}

int64_t Session::lastAccessNs() const noexcept {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_last_access_ns;
}

void Session::touch() noexcept {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_last_access_ns = nowNs();
}

// ---------------- SessionManager ----------------

SessionManager::SessionManager(SessionConfig cfg)
    : m_cfg(std::move(cfg)) {
}

std::string SessionManager::generateId() {
    // random_device 16B → 32 hex（纯 hex 天然免疫 CRLF 注入防线）
    static thread_local std::random_device rd;
    static const char* kHex = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (int i = 0; i < 16; ++i) {
        unsigned char b = static_cast<unsigned char>(rd());
        id += kHex[b >> 4];
        id += kHex[b & 0xF];
    }
    return id;
}

std::shared_ptr<Session> SessionManager::createSession() {
    // 洪水防护：超限拒建（未认证即可创建的资源必须有上限）
    if (m_sessions.size() >= m_cfg.maxSessions) {
        return nullptr;
    }
    auto s = std::make_shared<Session>(generateId());
    m_sessions.emplace(s->getId(), s);
    return s;
}

std::shared_ptr<Session> SessionManager::getSession(const HttpRequest& req) {
    std::shared_ptr<Session> existing = findSession(req);
    if (existing != nullptr) {
        return existing;  // 命中（findSession 内已 touch）
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    return createSession();
}

std::shared_ptr<Session> SessionManager::findSession(const HttpRequest& req) {
    auto cookie = req.getCookie(m_cfg.cookieName);
    if (!cookie.has_value()) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_sessions.find(std::string(*cookie));
    if (it == m_sessions.end()) {
        return nullptr;  // 未命中（伪造/过期已清扫）
    }
    // 惰性过期：超时即回收重建
    if (nowNs() - it->second->lastAccessNs() > static_cast<int64_t>(m_cfg.timeoutSec) * 1000'000'000) {
        m_sessions.erase(it);
        return nullptr;
    }
    it->second->touch();
    return it->second;
}

void SessionManager::attach(HttpResponse& resp, const std::shared_ptr<Session>& s, const SessionConfig& cfg) {
    Cookie c;
    c.name = cfg.cookieName;
    c.value = s->getId();
    c.path = "/";
    c.maxAge = static_cast<int64_t>(cfg.timeoutSec);
    c.httpOnly = true;
    c.sameSite = "Lax";
    resp.addCookie(std::move(c));
}

size_t SessionManager::sweep() {
    const int64_t deadline = static_cast<int64_t>(m_cfg.timeoutSec) * 1000'000'000;
    const int64_t now = nowNs();
    size_t reclaimed = 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (now - it->second->lastAccessNs() > deadline) {
            it = m_sessions.erase(it);
            ++reclaimed;
        } else {
            ++it;
        }
    }
    return reclaimed;
}

// ---------------- SessionFilter ----------------

SessionFilter::SessionFilter(std::shared_ptr<SessionManager> mgr)
    : m_mgr(std::move(mgr)) {
}

void SessionFilter::doFilter(const HttpRequest& req, HttpResponse& resp, FilterChain& chain) {
    // 建/回写 Cookie；handler 侧 mgr.getSession(req) 直取（本轮形态：req 上下文注入 H3）
    auto s = m_mgr->getSession(req);
    if (s == nullptr) {
        resp.SendError(HttpStatus::kSERVICE_UNAVAILABLE, "session limit reached");
        return;  // 短路（洪水防护）
    }
    SessionManager::attach(resp, s, m_mgr->config());  // 已有 cookie 时重复 attach 幂等覆盖
    chain.doFilter(req, resp);
}

}  // namespace bus::http
