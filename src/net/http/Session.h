//***********************************************
// name      : Session.h
// brief     : HTTP 会话管理（javax.servlet.http.HttpSession 对应物，库形态不内建）：
//             SessionManager（建/取/过期/洪水防护）+ SessionFilter（建+回写 Cookie）
// attention : manager 用户持有，生命周期独立于 HttpServer（无 Stop 时序耦合）；
//             handler 经 mgr.getSession(req) 直取（req 上下文注入通道登记 H3）；
//             同 session 并发请求语义等价 Tomcat——属性级不保证原子性
//***********************************************
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Macro.h"
#include "noncopyable.hpp"
#include "net/http/Filter.h"
#include "net/http/HttpMessage.h"

namespace bus::http {

/// @brief 会话配置（聚合体）
struct SessionConfig {
    std::string cookieName = "CODEBUSID";  ///< Cookie 名
    uint32_t timeoutSec = 1800;            ///< 空闲过期（秒）
    size_t maxSessions = 100000;           ///< 洪水防护上限（超限拒建，调用方 503）
};

/// @brief 单个会话（attrs 自带 mutex）
class Session : noncopyable {
 public:
    using ptr = std::shared_ptr<Session>;

    void setAttribute(std::string_view key, std::string_view value);
    /// @brief 取属性
    ATTR_NODISCARD
    std::optional<std::string_view> getAttribute(std::string_view key) const;
    void eraseAttribute(std::string_view key);
    ATTR_PURE_INLINE
    const std::string& getId() const noexcept {
        return m_id;
    }
    /// @brief 最后访问时刻（steady ns；惰性过期判定基准）
    ATTR_NODISCARD
    int64_t lastAccessNs() const noexcept;
    /// @brief 刷新访问时刻
    void touch() noexcept;

    /// @brief 构造（make_shared 需 public；语义上仅 SessionManager 应创建——
    ///        id 由 manager 的 generateId 产生）
    explicit Session(std::string id);

 private:
    /// @brief 会话 id（32 hex）
    const std::string m_id;
    /// @brief 属性锁（细粒度：会话级）
    mutable std::mutex m_mutex;
    /// @brief 最后访问时刻
    int64_t m_last_access_ns = 0;
    /// @brief 属性表（有序：遍历稳定）
    std::map<std::string, std::string> m_attrs;
};

/// @brief 会话管理器（用户持有）
class SessionManager : noncopyable {
 public:
    explicit SessionManager(SessionConfig cfg = {});

    /// @brief 取或建会话（无 cookie/未命中/过期 → 重建；洪水超限返回空）
    /// @return 空 = 超限（调用方回 503）
    std::shared_ptr<Session> getSession(const HttpRequest& req);
    /// @brief 只取不建（只读场景免新建副作用）
    std::shared_ptr<Session> findSession(const HttpRequest& req);
    /// @brief 新建会话时回写 Set-Cookie（HttpOnly/SameSite=Lax/Path=/Max-Age）
    static void attach(HttpResponse& resp, const std::shared_ptr<Session>& s, const SessionConfig& cfg);
    /// @brief 过期清扫（访问时已惰性判定，sweep 兜底回收内存）
    /// @return 回收条数
    size_t sweep();
    /// @brief 配置只读访问（Filter 回写 Cookie 用）
    ATTR_PURE_INLINE
    const SessionConfig& config() const noexcept {
        return m_cfg;
    }
    ATTR_PURE_INLINE
    size_t size() const noexcept {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_sessions.size();
    }

 private:
    std::shared_ptr<Session> createSession();
    static std::string generateId();

    /// @brief 配置
    const SessionConfig m_cfg;
    /// @brief 会话表锁
    mutable std::mutex m_mutex;
    /// @brief 会话表（key: session id）
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
};

/// @brief 会话过滤器：建/回写 Cookie；handler 侧 mgr.getSession(req) 直取会话
class SessionFilter : public Filter {
 public:
    explicit SessionFilter(std::shared_ptr<SessionManager> mgr);

    void doFilter(const HttpRequest& req, HttpResponse& resp, FilterChain& chain) override;

 private:
    /// @brief 管理器（共享持有）
    std::shared_ptr<SessionManager> m_mgr;
};

}  // namespace bus::http
