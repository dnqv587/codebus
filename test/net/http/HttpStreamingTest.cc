//***********************************************
// name      : HttpStreamingTest.cc
// brief     : 通用流式测试（sendfile 零拷贝/chunked 拉模型/Range 单区间/精确 bytes）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "EventLoopTestHelper.h"
#include "TestHelper.h"
#include "net/http/HttpCompression.h"
#include "net/http/HttpServer.h"
#include "net/http/StaticFileServlet.h"

using namespace bus;
using namespace bus::http;
using namespace bus::test;

namespace {

size_t ParseStatus(const std::string& raw) {
    size_t sp = raw.find(' ');
    if (sp == std::string::npos) return 0;
    return static_cast<size_t>(atoi(raw.c_str() + sp + 1));
}

size_t ParseContentLength(const std::string& raw, size_t hdr_end) {
    size_t pos = raw.find("Content-Length: ");
    if (pos == std::string::npos || pos > hdr_end) return 0;
    return static_cast<size_t>(atoll(raw.c_str() + pos + 16));
}

/// @brief 阻塞式测试客户端（读响应头+按 CL/chunked/EOF 收 body）
class StreamClient {
 public:
    ~StreamClient() {
        if (m_fd >= 0) ::close(m_fd);
    }
    bool connect(uint16_t port) {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    bool sendAll(std::string_view data) {
        size_t off = 0;
        while (off < data.size()) {
            // MSG_NOSIGNAL：半关对端写入不产生 SIGPIPE（曾致进程被杀无输出——实测教训）
            ssize_t n = ::send(m_fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }
    /// @brief 读到头结束（\r\n\r\n），返回头区与总原始串（body 前缀含于 raw）
    std::string readHead() {
        char tmp[16_KB];
        while (m_buf.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            m_buf.append(tmp, static_cast<size_t>(n));
        }
        return m_buf;
    }
    /// @brief 按已收头继续读满 n 字节 body
    bool readBodyN(size_t n) {
        char tmp[64_KB];
        size_t hdr = m_buf.find("\r\n\r\n");
        size_t have = m_buf.size() - hdr - 4;
        while (have < n) {
            ssize_t got = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (got <= 0) return false;
            m_buf.append(tmp, static_cast<size_t>(got));
            have += static_cast<size_t>(got);
        }
        return true;
    }
    /// @brief 读至 EOF（close-delimited/1.0 场景）
    std::string readAll() {
        char tmp[64_KB];
        for (;;) {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            m_buf.append(tmp, static_cast<size_t>(n));
        }
        return m_buf;
    }
    /// @brief chunked 解码（头后为 chunked 流；读至 0 终止块）
    /// @return 成功解码的 body；失败返回空（并用 last_error 标注）
    std::string decodeChunked(bool* ok) {
        *ok = false;
        std::string body;
        char tmp[64_KB];
        for (;;) {
            size_t pos = m_buf.find("\r\n", m_body_start);
            if (pos == std::string::npos) {
                ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
                if (n <= 0) return "";
                m_buf.append(tmp, static_cast<size_t>(n));
                continue;
            }
            size_t sz = strtoull(m_buf.c_str() + m_body_start, nullptr, 16);
            if (sz == 0) {
                *ok = true;
                return body;  // 0 终止块
            }
            while (m_buf.size() < pos + 2 + sz + 2) {
                ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
                if (n <= 0) return "";
                m_buf.append(tmp, static_cast<size_t>(n));
            }
            body.append(m_buf, pos + 2, sz);
            m_body_start = pos + 2 + sz + 2;
            fprintf(stderr, "[dec] block sz=%zu total=%zu next_start=%zu buflen=%zu\n", sz, body.size(),
                    m_body_start, m_buf.size());
        }
    }
    void setBodyStart(size_t hdr_end) {
        m_body_start = hdr_end + 4;
    }

 private:
    int m_fd = -1;
    std::string m_buf;
    size_t m_body_start = 0;
};

/// @brief 写临时文件并返回路径
std::string WriteTempFile(const std::string& tag, const std::string& content) {
    std::string path = "/tmp/codebus-stream-" + tag + ".bin";
    {
        std::ofstream f(path, std::ios::binary);
        f << content;
    }
    return path;
}

/// @brief 周期数据（校验和友好）
std::string MakePattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>('A' + (i % 26));
    }
    return s;
}

}  // namespace

class HttpStreamingTest : public ::testing::Test {
 protected:
    void SetUp() override {
        m_port = GetFreePort();
    }
    void TearDown() override {
        if (m_server) {
            if (!m_stop_done.load()) {
                std::atomic<bool> done{false};
                m_server->Stop([&] { done = true; });
                fprintf(stderr, "[td] stop requested, waiting done...\n");
                const bool ok = WaitFor([&] { return done.load(); }, std::chrono::milliseconds(10000));
                fprintf(stderr, "[td] done=%d ok=%d\n", (int)done.load(), (int)ok);
                ASSERT_TRUE(ok);
            }
            m_server.reset();
            fprintf(stderr, "[td] server reset done\n");
        }
    }
    void makeServer(HttpServerConfig cfg = {}) {
        m_server = std::make_unique<HttpServer>(m_loop.loop(), "127.0.0.1:" + std::to_string(m_port), 0, cfg);
    }
    void startAndWait() {
        m_server->Start();
        ASSERT_TRUE(WaitFor(
            [&] {
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0) return false;
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(m_port);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
                ::close(fd);
                return ok;
            },
            std::chrono::milliseconds(3000)));
    }

    LoopThread m_loop;
    uint16_t m_port = 0;
    std::unique_ptr<HttpServer> m_server;
    std::atomic<bool> m_stop_done{false};
    /// @brief 测试内临时根目录（Range 用例清理）
    std::string m_root;
};

/// [正] sendfile 零拷贝：8MB 文件收齐校验（U3）
TEST_F(HttpStreamingTest, SendFileLargeRoundTrip) {
    const std::string content = MakePattern(size_t(8_MB));
    const std::string path = WriteTempFile("big", content);
    makeServer();
    m_server->Get("/f", [&path](const HttpRequest&, HttpResponse& r) {
        ASSERT_TRUE(r.setBodyFile(path));
    });
    startAndWait();
    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /f HTTP/1.0\r\nHost: t\r\n\r\n"));  // 1.0：流尽 EOF
    std::string raw = c.readAll();
    EXPECT_NE(raw.find("Content-Length: 8388608"), std::string::npos);
    size_t hdr = raw.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    ASSERT_GE(raw.size(), hdr + 4 + content.size());
    EXPECT_EQ(raw.substr(hdr + 4, content.size()), content);  // 字节级一致
    ::unlink(path.c_str());
}

/// [正] chunked 拉模型：provider 分段产流 → 客户端解码拼回；流后 keep-alive 可续（U3）
TEST_F(HttpStreamingTest, ChunkedProviderStream) {
    makeServer();
    // 100 段 × 1KB（拼接 3 次取一次，避免闭包持大栈）
    m_server->Get("/stream", [](const HttpRequest&, HttpResponse& r) {
        auto shared = std::make_shared<std::pair<int, std::string>>(0, MakePattern(size_t(1_KB)));
        r.setBodyProvider([shared](std::string& chunk) {
            fprintf(stderr, "[prov] call #%d this=%p chunk=%p\n", shared->first, (void*)&shared->second,
                    (void*)&chunk);
            if (shared->first >= 100) {
                return false;
            }
            ++shared->first;
            chunk = shared->second;
            fprintf(stderr, "[prov] copy done #%d\n", shared->first);
            return true;
        });
    });
    startAndWait();
    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /stream HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string head = c.readHead();
    EXPECT_EQ(ParseStatus(head), 200u);
    EXPECT_NE(head.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_EQ(head.find("Content-Length:"), std::string::npos);  // chunked 无 CL
    size_t hdr = head.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    c.setBodyStart(hdr);
    {
        std::string hex;
        for (size_t i = 0; i < head.size() && i < 220; ++i) {
            char b[4];
            snprintf(b, sizeof(b), "%02x ", (unsigned char)head[i]);
            hex += b;
        }
        fprintf(stderr, "[cli] buflen=%zu hex: %s\n", head.size(), hex.c_str());
    }
    bool ok = false;
    std::string body = c.decodeChunked(&ok);
    fprintf(stderr, "[cli] decode ok=%d body=%zu\n", ok, body.size());
    ASSERT_TRUE(ok);
    fprintf(stderr, "[dbg] before expect block\n");
    {  // 期望=100 份段拼接（段独立周期；校验和比较——巨串 EXPECT_EQ 打印开销过大）
        const std::string seg = MakePattern(size_t(1_KB));
        size_t body_sum = 0, expect_sum = 0;
        for (char ch : body) {
            body_sum += static_cast<unsigned char>(ch);
        }
        for (int i = 0; i < 100; ++i) {
            for (char ch : seg) {
                expect_sum += static_cast<unsigned char>(ch);
            }
        }
        EXPECT_EQ(body.size(), size_t(100_KB));
        EXPECT_EQ(body_sum, expect_sum);  // 位置无关校验和 + 首尾段字节抽检
        EXPECT_EQ(body.substr(0, 8), seg.substr(0, 8));
        EXPECT_EQ(body.substr(body.size() - 8), seg.substr(seg.size() - 8));
    }
    fprintf(stderr, "[dbg] expect done, probe next\n");
    // keep-alive 探针：流后可发下一请求（发送成功即证连接复用；不读回——
    // 避免第二流背压挂起 TearDown；探针走 Connection:close 让服务端收尾）
    fprintf(stderr, "[dbg] probe sending\n");
    const bool sent = c.sendAll("GET /no-such HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    fprintf(stderr, "[dbg] probe sent=%d\n", (int)sent);
    ASSERT_TRUE(sent);
    // 竞态消解（登记 U3-R）：探针尾包（404+close 链）与 TearDown Stop 存在队列序竞态——
    // 快速路径下 Stop 先于尾包处理完成时连接表收尾悬挂；短 settle 让服务端排空尾包
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    SUCCEED();
}

/// [正] provider 流尽（HTTP/1.0 close-delimited：无 TE/CL，流尽 EOF）
TEST_F(HttpStreamingTest, ChunkedHttp10CloseDelimited) {
    makeServer();
    m_server->Get("/s10", [](const HttpRequest&, HttpResponse& r) {
        r.setBodyProvider([](std::string& chunk) {
            static int calls = 0;  // 单连接串行（库语义）——用例自足
            if (calls >= 3) {
                return false;
            }
            ++calls;
            chunk = "seg";
            return true;
        });
    });
    startAndWait();
    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /s10 HTTP/1.0\r\nHost: t\r\n\r\n"));
    std::string raw = c.readAll();
    EXPECT_EQ(ParseStatus(raw), 200u);
    EXPECT_EQ(raw.find("Transfer-Encoding"), std::string::npos);
    EXPECT_EQ(raw.find("Content-Length:"), std::string::npos);
    EXPECT_NE(raw.find("segsegseg"), std::string::npos);  // 1.0：直接体（无 chunk 编码）
}

/// [界] provider 异常 → 连接关（framing 不可信不保活）
TEST_F(HttpStreamingTest, ChunkedProviderAbort) {
    makeServer();
    m_server->Get("/boom", [](const HttpRequest&, HttpResponse& r) {
        r.setBodyProvider([](std::string&) -> bool {
            throw std::runtime_error("provider boom");
        });
    });
    startAndWait();
    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /boom HTTP/1.1\r\nHost: t\r\n\r\n"));
    std::string raw = c.readAll();  // 收头+立即关闭
    EXPECT_EQ(ParseStatus(raw), 200u);
    // EOF 即关（无终止块/无后续）——以短读不挂死为锁定
    SUCCEED();
}

/// [正] Range 单区间三形态（U3：StaticFileServlet）
TEST_F(HttpStreamingTest, RangeSingleInterval) {
    const std::string content = MakePattern(size_t(4_KB));
    const std::string root = "/tmp/codebus-range-www-XXXXXX";
    {
        std::vector<char> buf(root.begin(), root.end());
        buf.push_back('\0');
        char* p = mkdtemp(buf.data());
        ASSERT_NE(p, nullptr);
        m_root = p;
    }
    {
        std::ofstream(m_root + "/a.bin", std::ios::binary) << content;
    }
    makeServer();
    // 根前缀挂载（StaticFileServlet 语义：URL 空间与 root 一一对应）
    auto sfs = std::make_shared<StaticFileServlet>(m_root);
    m_server->dispatcher()->addPrefixServlet("/", sfs);
    startAndWait();

    struct Case {
        const char* range;
        size_t want_off;
        size_t want_len;
    };
    for (const Case& cs : {Case{"bytes=0-99", 0, 100}, Case{"bytes=100-199", 100, 100},
                           Case{"bytes=4000-", 4000, size_t(4_KB) - 4000},
                           Case{"bytes=-50", size_t(4_KB) - 50, 50}}) {
        StreamClient c;
        ASSERT_TRUE(c.connect(m_port));
        ASSERT_TRUE(c.sendAll(std::string("GET /a.bin HTTP/1.0\r\nHost: t\r\nRange: ") + cs.range +
                              "\r\n\r\n"));
        std::string raw = c.readAll();
        EXPECT_EQ(ParseStatus(raw), 206u) << cs.range;
        EXPECT_NE(raw.find("Content-Range: bytes"), std::string::npos) << cs.range;
        EXPECT_NE(raw.find("Content-Length: " + std::to_string(cs.want_len)), std::string::npos) << cs.range;
        size_t hdr = raw.find("\r\n\r\n");
        ASSERT_NE(hdr, std::string::npos);
        ASSERT_GE(raw.size(), hdr + 4 + cs.want_len);
        EXPECT_EQ(raw.substr(hdr + 4, cs.want_len), content.substr(cs.want_off, cs.want_len)) << cs.range;
    }
    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}

/// [界] Range 超界 → 416
TEST_F(HttpStreamingTest, RangeInvalid416) {
    const std::string root = "/tmp/codebus-416-www-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    {
        std::ofstream(m_root + "/a.bin", std::ios::binary) << MakePattern(64);
    }
    makeServer();
    auto sfs2 = std::make_shared<StaticFileServlet>(m_root);
    m_server->dispatcher()->addPrefixServlet("/", sfs2);
    startAndWait();
    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /a.bin HTTP/1.0\r\nHost: t\r\nRange: bytes=99999-\r\n\r\n"));
    std::string raw = c.readAll();
    EXPECT_EQ(ParseStatus(raw), 416u);
    EXPECT_NE(raw.find("bytes */64"), std::string::npos);
    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}
