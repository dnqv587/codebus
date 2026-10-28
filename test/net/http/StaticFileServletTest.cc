//***********************************************
// name      : StaticFileServletTest.cc
// brief     : 静态文件 Servlet 测试（命中/MIME/304/防遍历/Cache-Control）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <unistd.h>

#include "net/http/StaticFileServlet.h"

using namespace bus;
using namespace bus::http;

namespace {

/// @brief 临时目录 fixture：建 www/{a.html,b.js,sub/index.html}
class TempWebRoot {
 public:
    TempWebRoot() {
        m_dir = "/tmp/codebus-static-test-XXXXXX";
        std::vector<char> buf(m_dir.begin(), m_dir.end());
        buf.push_back('\0');
        char* p = mkdtemp(buf.data());
        m_dir = p;
        mkdir((m_dir + "/sub").c_str(), 0755);
        Write("a.html", "<h1>hi</h1>");
        Write("b.js", "console.log(1)");
        Write("sub/index.html", "INDEX");
    }
    ~TempWebRoot() {
        for (const std::string& f : {"a.html", "b.js", "sub/index.html"}) {
            ::unlink((m_dir + "/" + f).c_str());
        }
        ::rmdir((m_dir + "/sub").c_str());
        ::rmdir(m_dir.c_str());
    }
    void Write(const std::string& rel, const std::string& content) {
        std::ofstream(m_dir + "/" + rel) << content;
    }
    const std::string& dir() const {
        return m_dir;
    }

 private:
    std::string m_dir;
};

HttpRequest MakeGet(std::string_view path) {
    HttpRequest req;
    req.setMethod(HttpMethod::kGET);
    req.setPath(std::string(path));
    return req;
}

}  // namespace

/// [正] 命中与 MIME
TEST(StaticFileServletTest, StaticHitMimeTypes) {
    TempWebRoot root;
    StaticFileServlet sfs(root.dir());
    {
        HttpResponse resp;
        sfs.Handle(MakeGet("/a.html"), resp);
        EXPECT_EQ(resp.getStatus(), HttpStatus::kOK);
        // 文件源：body 经流发送（getBody 空），断 CL=11 与 Content-Type
        EXPECT_GE(resp.bodyFileFd(), 0);
        HttpRequest probe = MakeGet("/a.html");
        EXPECT_NE(resp.DumpHead(probe).find("Content-Length: 11\r\n"), std::string::npos);
        ASSERT_TRUE(resp.getHeader("Content-Type").has_value());
        EXPECT_EQ(*resp.getHeader("Content-Type"), "text/html");
    }
    {
        HttpResponse resp;
        sfs.Handle(MakeGet("/b.js"), resp);
        ASSERT_TRUE(resp.getHeader("Content-Type").has_value());
        EXPECT_EQ(*resp.getHeader("Content-Type"), "application/javascript");
    }
}

/// [正] 目录 → index.html
TEST(StaticFileServletTest, StaticDirectoryIndex) {
    TempWebRoot root;
    StaticFileServlet sfs(root.dir());
    HttpResponse resp;
    sfs.Handle(MakeGet("/sub/"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kOK);
    EXPECT_GE(resp.bodyFileFd(), 0);  // index.html 文件源（"INDEX"=5B）
}

/// [界] 不存在 → 404
TEST(StaticFileServletTest, Static404) {
    TempWebRoot root;
    StaticFileServlet sfs(root.dir());
    HttpResponse resp;
    sfs.Handle(MakeGet("/nope.txt"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kNOT_FOUND);
}

/// [界] 防遍历：symlink 拒绝（默认）；dot-segment 在 Dispatcher 层已 400（本层不重复测）
TEST(StaticFileServletTest, StaticSymlinkRejected) {
    TempWebRoot root;
    // 在 root 内放一个指向 /etc 的 symlink
    std::string link = root.dir() + "/evil";
    ::symlink("/etc", link.c_str());
    StaticFileServlet sfs(root.dir());
    HttpResponse resp;
    sfs.Handle(MakeGet("/evil/passwd"), resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kNOT_FOUND);  // symlink 默认拒
    ::unlink(link.c_str());
}

/// [正] If-Modified-Since 匹配 mtime → 304
TEST(StaticFileServletTest, Static304) {
    TempWebRoot root;
    StaticFileServlet sfs(root.dir());
    struct ::stat st;
    ::stat((root.dir() + "/a.html").c_str(), &st);
    // 构造 RFC1123 时间（复用框架同款查表逻辑：经 gmtime 手拼）
    struct tm t;
    gmtime_r(&st.st_mtime, &t);
    static const char* kW[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kM[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char date[40];
    snprintf(date, sizeof(date), "%s, %02d %s %04d %02d:%02d:%02d GMT", kW[t.tm_wday], t.tm_mday,
             kM[t.tm_mon], t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
    HttpRequest req = MakeGet("/a.html");
    req.addHeaderRaw("If-Modified-Since", date);
    HttpResponse resp;
    sfs.Handle(req, resp);
    EXPECT_EQ(resp.getStatus(), HttpStatus::kNOT_MODIFIED);
}

/// [正] Cache-Control max-age
TEST(StaticFileServletTest, StaticCacheControl) {
    TempWebRoot root;
    StaticFileServlet sfs(root.dir(), StaticFileConfig{.maxAgeSec = 3600});
    HttpResponse resp;
    sfs.Handle(MakeGet("/a.html"), resp);
    ASSERT_TRUE(resp.getHeader("Cache-Control").has_value());
    EXPECT_EQ(*resp.getHeader("Cache-Control"), "max-age=3600");
}
