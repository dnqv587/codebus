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
    // Range 单区间（U3）：bytes=a-b / a- / -n；无 Range 头走全量 200
    auto range = req.getHeader("range");
    if (range.has_value()) {
        uint64_t total = static_cast<uint64_t>(st.st_size);
        uint64_t off = 0;
        uint64_t len = 0;
        std::string rv(*range);
        if (rv.rfind("bytes=", 0) == 0 && rv.find(',') == std::string::npos) {
            std::string spec = rv.substr(6);
            size_t dash = spec.find('-');
            if (dash != std::string::npos) {
                std::string first = spec.substr(0, dash);
                std::string last = spec.substr(dash + 1);
                bool ok = false;
                if (first.empty()) {
                    uint64_t n = strtoull(last.c_str(), nullptr, 10);  // bytes=-n：末 n 字节
                    if (!last.empty() && n > 0 && n <= total) {
                        off = total - n;
                        len = n;
                        ok = true;
                    }
                } else {
                    uint64_t a = strtoull(first.c_str(), nullptr, 10);
                    if (!first.empty() && a < total) {
                        off = a;
                        if (last.empty()) {
                            len = total - a;  // bytes=a-
                            ok = true;
                        } else {
                            uint64_t b = strtoull(last.c_str(), nullptr, 10);
                            if (b >= a) {
                                len = std::min<uint64_t>(b - a + 1, total - a);
                                ok = true;
                            }
                        }
                    }
                }
                if (ok && resp.setBodyFileRange(real, off, len)) {
                    resp.setStatus(HttpStatus::kPARTIAL_CONTENT);
                    resp.setHeader("Content-Range", "bytes " + std::to_string(off) + "-" +
                                                        std::to_string(off + len - 1) + "/" +
                                                        std::to_string(total));
                    return;
                }
            }
        }
        // 非法/超界 → 416（多区间 200 全量登记 H5）
        resp.setHeader("Content-Range", "bytes */" + std::to_string(total));
        resp.SendError(HttpStatus::kRANGE_NOT_SATISFIABLE);
        return;
    }
    if (!resp.setBodyFile(real)) {
        resp.SendError(HttpStatus::kNOT_FOUND);
    }
}

}  // namespace bus::http
