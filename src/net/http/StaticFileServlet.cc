//***********************************************
// name      : StaticFileServlet.cc
// brief     : 静态文件 Servlet 实现
// attention : None
//***********************************************
#include "net/http/StaticFileServlet.h"

#include <climits>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

#include "log/Logger.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("system");

namespace {

/// @brief 构造期解析根目录真实路径（失败抛出）
std::string ResolveRoot(const std::string& root) {
    char resolved[PATH_MAX];
    if (::realpath(root.c_str(), resolved) == nullptr) {
        throw std::runtime_error("StaticFileServlet: root not found: " + root);
    }
    return resolved;
}

}  // namespace

StaticFileServlet::StaticFileServlet(std::string root, StaticFileConfig cfg)
    : m_root_real(ResolveRoot(root)),
      m_cfg(cfg) {
}

std::string_view StaticFileServlet::MimeTypeFor(std::string_view path) {
    static const struct {
        const char* ext;
        const char* mime;
    } kTable[] = {
        {".html", "text/html"},       {".htm", "text/html"},   {".css", "text/css"},
        {".js", "application/javascript"}, {".mjs", "application/javascript"},
        {".json", "application/json"}, {".txt", "text/plain"}, {".xml", "application/xml"},
        {".svg", "image/svg+xml"},     {".png", "image/png"},   {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},       {".gif", "image/gif"},   {".ico", "image/x-icon"},
        {".webp", "image/webp"},       {".woff", "font/woff"},  {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},          {".otf", "font/otf"},    {".pdf", "application/pdf"},
        {".zip", "application/zip"},   {".gz", "application/gzip"},
        {".mp4", "video/mp4"},         {".webm", "video/webm"}, {".mp3", "audio/mpeg"},
    };
    size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return "application/octet-stream";
    }
    std::string_view ext = path.substr(dot);
    for (const auto& e : kTable) {
        if (ext == e.ext) {
            return e.mime;
        }
    }
    return "application/octet-stream";
}

time_t StaticFileServlet::ParseHttpDate(std::string_view v) noexcept {
    // RFC1123："Sun, 06 Nov 1994 08:49:37 GMT"（仅支持本框架产出的格式）
    static const char* kMonth[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm t{};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[8] = {};
    if (sscanf(std::string(v).c_str(), "%*3s, %2d %3s %4d %2d:%2d:%2d", &day, mon, &year, &hh, &mm,
               &ss) != 6) {
        return -1;
    }
    int mi = -1;
    for (int i = 0; i < 12; ++i) {
        if (strncmp(mon, kMonth[i], 3) == 0) {
            mi = i;
            break;
        }
    }
    if (mi < 0) {
        return -1;
    }
    t.tm_mday = day;
    t.tm_mon = mi;
    t.tm_year = year - 1900;
    t.tm_hour = hh;
    t.tm_min = mm;
    t.tm_sec = ss;
    return timegm(&t);
}

void StaticFileServlet::Handle(const HttpRequest& req, HttpResponse& resp) {
    // 仅 GET/HEAD
    if (req.getMethod() != HttpMethod::kGET && req.getMethod() != HttpMethod::kHEAD) {
        resp.setHeader("Allow", "GET, HEAD");
        resp.SendError(HttpStatus::kMETHOD_NOT_ALLOWED);
        return;
    }
    std::string rel(req.getPath());
    if (rel.empty() || rel.back() == '/') {
        rel += "index.html";  // 目录 → index.html
    }

    // 防遍历第一线：realpath 前缀校验（symlink 指向 root 外在此即拒）
    std::string full = m_root_real + rel;
    char resolved[PATH_MAX];
    if (::realpath(full.c_str(), resolved) == nullptr) {
        resp.SendError(HttpStatus::kNOT_FOUND);
        return;
    }
    std::string real(resolved);
    if (real.compare(0, m_root_real.size(), m_root_real) != 0 ||
        (real.size() > m_root_real.size() && real[m_root_real.size()] != '/') &&
            real != m_root_real) {
        resp.SendError(HttpStatus::kNOT_FOUND);  // 逃逸 root
        return;
    }
    // 第二线：root 内 symlink 按开关拒绝（lstat 不跟随）
    if (!m_cfg.allowSymlink) {
        struct ::stat lst;
        if (::lstat(full.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode)) {
            LOG_FMT_WARN(g_logger, "StaticFileServlet: symlink rejected: %.*s",
                         static_cast<int>(rel.size()), rel.data());
            resp.SendError(HttpStatus::kNOT_FOUND);
            return;
        }
    }
    struct ::stat st;
    if (::stat(real.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        resp.SendError(HttpStatus::kNOT_FOUND);
        return;
    }
    resp.setHeader("Content-Type", MimeTypeFor(rel));
    if (m_cfg.maxAgeSec > 0) {
        resp.setHeader("Cache-Control", "max-age=" + std::to_string(m_cfg.maxAgeSec));
    }
    // If-Modified-Since 匹配（未修改）→ 304
    auto ims = req.getHeader("if-modified-since");
    if (ims.has_value()) {
        time_t since = ParseHttpDate(*ims);
        if (since >= 0 && st.st_mtime <= since) {
            resp.setStatus(HttpStatus::kNOT_MODIFIED);
            return;
        }
    }
    if (!resp.setBodyFile(real)) {
        resp.SendError(HttpStatus::kNOT_FOUND);
    }
}

}  // namespace bus::http
