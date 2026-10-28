//***********************************************
// name      : StaticFileServlet.h
// brief     : 静态文件 Servlet（库形态）：root 挂载 + MIME 表 + If-Modified-Since/304
//             + Cache-Control + 防遍历（realpath 前缀校验 + symlink 拒绝开关）
// attention : dot-segment 越界由 Dispatcher 前置 400（本层不重复）；目录 → index.html；
//             Range 不支持（无 Range 头回 200 全量，登记 H3）
//***********************************************
#pragma once
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "net/http/HttpMessage.h"
#include "net/http/Servlet.h"

namespace bus::http {

/// @brief 静态文件配置（聚合体）
struct StaticFileConfig {
    uint32_t maxAgeSec = 0;      ///< Cache-Control max-age（0=不输出）
    bool allowSymlink = false;   ///< 符号链接拒绝开关（默认拒）
};

/// @brief 静态文件处理器（dispatcher 注册形态：addPrefixServlet("/static/*", ...)）
class StaticFileServlet : public Servlet {
 public:
    /// @param root 根目录（构造期 realpath 解析；不存在抛 std::runtime_error）
    explicit StaticFileServlet(std::string root, StaticFileConfig cfg = {});

    void Handle(const HttpRequest& req, HttpResponse& resp) override;
    ATTR_NODISCARD
    std::string_view getName() const noexcept override {
        return "StaticFile";
    }

 private:
    /// @brief 扩展名 → Content-Type（~25 项表；未知 application/octet-stream）
    static std::string_view MimeTypeFor(std::string_view path);
    /// @brief 解析 RFC1123 时间（If-Modified-Since；失败返回 -1）
    static time_t ParseHttpDate(std::string_view v) noexcept;

    /// @brief 根目录真实路径（构造期解析）
    const std::string m_root_real;
    /// @brief 配置
    const StaticFileConfig m_cfg;
};

}  // namespace bus::http
