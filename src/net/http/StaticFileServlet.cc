//***********************************************
// name      : StaticFileServlet.cc
// brief     : 静态文件 Servlet 实现
// attention : None
//***********************************************
#include "net/http/StaticFileServlet.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log/Logger.h"
#include "net/http/HttpUtil.h"
#include "noncopyable.hpp"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("sys");

namespace {

/// @brief 构造期解析根目录真实路径（失败抛出）
std::string ResolveRoot(const std::string& root) {
    char resolved[PATH_MAX];
    if (::realpath(root.c_str(), resolved) == nullptr) {
        throw std::runtime_error("StaticFileServlet: root not found: " + root);
    }
    return resolved;
}

/// @brief If-None-Match 命中判定（RFC 9110 §13.1.2：列表逐项弱比较，W/ 前缀等价，* 通配）
/// @param header 原始头值（逗号分隔 entity-tag 列表或 *）
/// @param etag 本资源的强 ETag（含引号）
/// @return 任一项命中返回 true
bool IfNoneMatchHits(std::string_view header, std::string_view etag) {
    size_t pos = 0;
    while (pos < header.size()) {
        size_t comma = header.find(',', pos);
        std::string_view item = header.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (item == "*") {
            return true;
        }
        if (item.rfind("W/", 0) == 0) {
            item.remove_prefix(2);  // 弱比较：W/ 前缀剥离后比对
        }
        if (item == etag) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

/// @brief fd 守卫（H5.1 TOCTOU）：早退路径自动关闭；release 移交所有权给响应体
struct FdGuard : noncopyable {
    explicit FdGuard(int f) : fd(f) {}
    int fd;
    ~FdGuard() noexcept {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    int release() {
        int f = fd;
        fd = -1;
        return f;
    }
};

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
    // H5.1 Q38：挂载前缀剥离——尾段捕获非空时以尾段作 docroot 相对路径（通配挂载）；
    // 尾段空（根挂载/精确路由）维持全路径语义
    std::string rel;
    const std::string_view tail = req.getPathTail();
    if (!tail.empty()) {
        rel = "/" + std::string(tail);
    } else {
        rel = std::string(req.getPath());
    }
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
    // H5.1 TOCTOU：先 open 后 fstat——文件身份来自已打开 fd（校验/ETag/size 同源），
    // 闭合「校验的 file ≠ 写出的 file」窗口；O_NOFOLLOW 守 realpath→open 窗口末分量替换
    const int flags = O_RDONLY | (m_cfg.allowSymlink ? 0 : O_NOFOLLOW);
    FdGuard guard{::open(real.c_str(), flags)};
    if (guard.fd < 0) {
        resp.SendError(HttpStatus::kNOT_FOUND);
        return;
    }
    struct ::stat st;
    if (::fstat(guard.fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        resp.SendError(HttpStatus::kNOT_FOUND);
        return;
    }
    // H5.1 Q38：强 ETag（mtime+size 十六进制，nginx 同款）+ Last-Modified 输出
    char etag_buf[64];
    snprintf(etag_buf, sizeof(etag_buf), "\"%lx-%lx\"", static_cast<unsigned long>(st.st_mtime),
             static_cast<unsigned long>(st.st_size));
    resp.setHeader("ETag", etag_buf);
    resp.setHeader("Last-Modified", FormatHttpDate(st.st_mtime));
    resp.setHeader("Content-Type", MimeTypeFor(rel));
    if (m_cfg.maxAgeSec > 0) {
        resp.setHeader("Cache-Control", "max-age=" + std::to_string(m_cfg.maxAgeSec));
    }
    // H5.1 Q38：条件验证 RFC 9110 优先级——If-None-Match 存在时先于 If-Modified-Since；
    // INM 命中 → 304（先于 Range 处理），未命中 → 跳过 IMS 走全量
    auto inm = req.getHeader("if-none-match");
    if (inm.has_value()) {
        if (IfNoneMatchHits(*inm, etag_buf)) {
            resp.setStatus(HttpStatus::kNOT_MODIFIED);
            return;
        }
    } else {
        // If-Modified-Since 匹配（未修改）→ 304（ETag 缺失时的回退腿）
        auto ims = req.getHeader("if-modified-since");
        if (ims.has_value()) {
            time_t since = ParseHttpDate(*ims);
            if (since >= 0 && st.st_mtime <= since) {
                resp.setStatus(HttpStatus::kNOT_MODIFIED);
                return;
            }
        }
    }
    // Range 单区间（U3）：bytes=a-b / a- / -n；无 Range 头走全量 200。
    // 多区间（含逗号）→ 忽略 Range 回落全量 200（H4.2；RFC 允许忽略 Range，不做 multipart/byteranges）
    auto range = req.getHeader("range");
    if (range.has_value() && range->find(',') == std::string::npos) {
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
                if (ok && resp.setBodyFileRange(guard.release(), off, len)) {
                    resp.setStatus(HttpStatus::kPARTIAL_CONTENT);
                    resp.setHeader("Content-Range", "bytes " + std::to_string(off) + "-" +
                                                        std::to_string(off + len - 1) + "/" +
                                                        std::to_string(total));
                    return;
                }
            }
        }
        // 非法/超界单区间 → 416（多区间已在上方回落全量 200）
        resp.setHeader("Content-Range", "bytes */" + std::to_string(total));
        resp.SendError(HttpStatus::kRANGE_NOT_SATISFIABLE);
        return;
    }
    if (!resp.setBodyFile(guard.release())) {
        resp.SendError(HttpStatus::kNOT_FOUND);
    }
}

}  // namespace bus::http
