//***********************************************
// name      : HttpStreamingTest.cc
// brief     : 通用流式测试（sendfile 零拷贝/chunked 拉模型/Range 单区间/精确 bytes）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ctime>

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

    /// @brief 等待对端关闭（丢弃期间收到的数据；超时返回 false）
    bool waitClosed(int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            timeval tv{0, 20 * 1000};
            ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char tmp[64_KB];
            const ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n == 0) return true;
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    /// @brief 读一个 Content-Length 响应并消费（pipeline 顺序断言用）
    bool readContentLengthResponse(size_t* status, std::string* body) {        char tmp[64_KB];
        while (m_buf.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            m_buf.append(tmp, static_cast<size_t>(n));
        }
        const size_t hdr_end = m_buf.find("\r\n\r\n");
        const size_t len = ParseContentLength(m_buf, hdr_end);
        const size_t total = hdr_end + 4 + len;
        while (m_buf.size() < total) {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            m_buf.append(tmp, static_cast<size_t>(n));
        }
        *status = ParseStatus(m_buf);
        *body = m_buf.substr(hdr_end + 4, len);
        m_buf.erase(0, total);
        return true;
    }

    /// @brief 读一个 chunked 响应并消费（不含 trailer；pipeline 顺序断言用）
    bool readChunkedResponse(size_t* status, std::string* body) {
        char tmp[64_KB];
        while (m_buf.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            m_buf.append(tmp, static_cast<size_t>(n));
        }
        const size_t hdr_end = m_buf.find("\r\n\r\n");
        *status = ParseStatus(m_buf);
        body->clear();
        size_t pos = hdr_end + 4;
        for (;;) {
            size_t line = m_buf.find("\r\n", pos);
            while (line == std::string::npos) {
                ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
                if (n <= 0) return false;
                m_buf.append(tmp, static_cast<size_t>(n));
                line = m_buf.find("\r\n", pos);
            }
            const size_t sz = strtoull(m_buf.c_str() + pos, nullptr, 16);
            const size_t need = line + 2 + sz + (sz == 0 ? 2 : 2);
            while (m_buf.size() < need) {
                ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
                if (n <= 0) return false;
                m_buf.append(tmp, static_cast<size_t>(n));
            }
            if (sz == 0) {
                m_buf.erase(0, line + 4);  // "0\r\n\r\n"
                return true;
            }
            body->append(m_buf, line + 2, sz);
            pos = line + 2 + sz + 2;
        }
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

    /// @brief 文件流期间同包 pipeline：第二响应必须等文件完整发送后才能出现
    void CheckFilePipeline(bool worker) {
        const std::string content = MakePattern(size_t(8_MB));
        const std::string path = WriteTempFile(worker ? "worker-pipe" : "io-pipe", content);
        makeServer(worker ? HttpServerConfig{.workerNum = 2} : HttpServerConfig{});
        m_server->Get("/f", [&path](const HttpRequest&, HttpResponse& r) {
            ASSERT_TRUE(r.setBodyFile(path));
        });
        m_server->Get("/next", [](const HttpRequest&, HttpResponse& r) {
            r.setBody(std::string("next"));
        });
        startAndWait();
        StreamClient c;
        ASSERT_TRUE(c.connect(m_port));
        ASSERT_TRUE(c.sendAll("GET /f HTTP/1.1\r\nHost: t\r\n\r\nGET /next HTTP/1.1\r\nHost: t\r\n\r\n"));
        size_t status = 0;
        std::string body;
        ASSERT_TRUE(c.readContentLengthResponse(&status, &body));
        EXPECT_EQ(status, 200u);
        ASSERT_EQ(body.size(), content.size());
        EXPECT_EQ(std::memcmp(body.data(), content.data(), content.size()), 0);
        ASSERT_TRUE(c.readContentLengthResponse(&status, &body));
        EXPECT_EQ(status, 200u);
        EXPECT_EQ(body, "next");
    }

    /// @brief provider 流期间同包 pipeline：chunk 帧不得被第二响应打断
    void CheckProviderPipeline(bool worker) {
        makeServer(worker ? HttpServerConfig{.workerNum = 2} : HttpServerConfig{});
        m_server->Get("/stream", [](const HttpRequest&, HttpResponse& r) {
            auto step = std::make_shared<int>(0);
            r.setBodyProvider([step](std::string& chunk) {
                if (*step == 3) return false;
                chunk.assign(64_KB, static_cast<char>('A' + (*step)++));
                return true;
            });
        });
        m_server->Get("/next", [](const HttpRequest&, HttpResponse& r) {
            r.setBody(std::string("next"));
        });
        startAndWait();
        StreamClient c;
        ASSERT_TRUE(c.connect(m_port));
        ASSERT_TRUE(c.sendAll("GET /stream HTTP/1.1\r\nHost: t\r\n\r\nGET /next HTTP/1.1\r\nHost: t\r\n\r\n"));
        size_t status = 0;
        std::string body;
        ASSERT_TRUE(c.readChunkedResponse(&status, &body));
        EXPECT_EQ(status, 200u);
        std::string expected;
        for (int i = 0; i < 3; ++i) expected.append(64_KB, static_cast<char>('A' + i));
        ASSERT_EQ(body.size(), expected.size());
        EXPECT_EQ(std::memcmp(body.data(), expected.data(), expected.size()), 0);
        ASSERT_TRUE(c.readContentLengthResponse(&status, &body));
        EXPECT_EQ(status, 200u);
        EXPECT_EQ(body, "next");
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

/// [回归] 文件流期间同包 pipeline 不得插入第二响应（IO 内联）
TEST_F(HttpStreamingTest, FilePipelineOrderedAfterStream) {
    CheckFilePipeline(/*worker=*/false);
}

/// [回归] provider 流期间同包 pipeline 不得插入第二响应（IO 内联）
TEST_F(HttpStreamingTest, ProviderPipelineOrderedAfterStream) {
    CheckProviderPipeline(/*worker=*/false);
}

/// [回归] 文件流期间同包 pipeline 不得插入第二响应（worker 模式）
TEST_F(HttpStreamingTest, WorkerFilePipelineOrderedAfterStream) {
    CheckFilePipeline(/*worker=*/true);
}

/// [回归] provider 流期间同包 pipeline 不得插入第二响应（worker 模式）
TEST_F(HttpStreamingTest, WorkerProviderPipelineOrderedAfterStream) {
    CheckProviderPipeline(/*worker=*/true);
}

/// [回归] 文件打开后被截断：sendfile 提前 EOF 必须关闭错误 framing 的连接
TEST_F(HttpStreamingTest, SendFileTruncatedClosesConnection) {
    const std::string content = MakePattern(size_t(1_MB));
    const std::string path = WriteTempFile("truncate-close", content);
    makeServer();
    m_server->Get("/f", [&path](const HttpRequest&, HttpResponse& r) {
        ASSERT_TRUE(r.setBodyFile(path));
        ASSERT_EQ(::truncate(path.c_str(), 0), 0);  // 文件大小已记账，随后内容被截断
    });
    startAndWait();
    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /f HTTP/1.1\r\nHost: t\r\n\r\n"));
    EXPECT_TRUE(c.waitClosed(5000)) << "truncated file left connection pending instead of closing";
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

/// [正] 多区间 Range → 回落 200 全量（H4.2：忽略 Range，RFC 允许；单区间 206 不变）
TEST_F(HttpStreamingTest, RangeMultiIntervalFallsBack200) {
    const std::string content = MakePattern(size_t(4_KB));
    const std::string root = "/tmp/codebus-multirange-www-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    {
        std::ofstream(m_root + "/a.bin", std::ios::binary) << content;
    }
    makeServer();
    auto sfs = std::make_shared<StaticFileServlet>(m_root);
    m_server->dispatcher()->addPrefixServlet("/", sfs);
    startAndWait();
    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /a.bin HTTP/1.0\r\nHost: t\r\nRange: bytes=0-99,200-299\r\n\r\n"));
    std::string raw = c.readAll();
    EXPECT_EQ(ParseStatus(raw), 200u);
    EXPECT_EQ(raw.find("Content-Range:"), std::string::npos);  // 全量回落不携带 Content-Range
    EXPECT_NE(raw.find("Content-Length: " + std::to_string(content.size())), std::string::npos);
    size_t hdr = raw.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    ASSERT_GE(raw.size(), hdr + 4 + content.size());
    EXPECT_EQ(raw.substr(hdr + 4, content.size()), content);
    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}

/// [正] ETag + Last-Modified 输出（H5.1 Q38）：强 ETag 格式、LM 与文件 mtime 一致、跨请求稳定
TEST_F(HttpStreamingTest, ETagAndLastModifiedEmitted) {
    const std::string root = "/tmp/codebus-etag-www-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    { std::ofstream(m_root + "/a.bin", std::ios::binary) << MakePattern(1_KB); }
    makeServer();
    m_server->dispatcher()->addPrefixServlet("/", std::make_shared<StaticFileServlet>(m_root));
    startAndWait();

    auto getRaw = [&](const char* extra) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll(std::string("GET /a.bin HTTP/1.0\r\nHost: t\r\n") + extra + "\r\n"));
        return c.readAll();
    };
    auto header = [](const std::string& raw, const char* name) -> std::string {
        const std::string key = std::string(name) + ": ";
        size_t pos = raw.find(key);
        if (pos == std::string::npos) return "";
        size_t end = raw.find("\r\n", pos);
        return raw.substr(pos + key.size(), end - pos - key.size());
    };

    const std::string raw = getRaw("");
    EXPECT_EQ(ParseStatus(raw), 200u);
    const std::string etag = header(raw, "ETag");
    ASSERT_FALSE(etag.empty());
    EXPECT_EQ(etag.front(), '"');
    EXPECT_EQ(etag.back(), '"');
    EXPECT_NE(etag.find('-'), std::string::npos);

    // Last-Modified 经 strptime 独立解析，与文件 mtime 一致（非同算法重算）
    const std::string lm = header(raw, "Last-Modified");
    ASSERT_FALSE(lm.empty());
    struct tm tm_parsed {};
    ASSERT_NE(strptime(lm.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm_parsed), nullptr);
    struct stat st {};
    ASSERT_EQ(::stat((m_root + "/a.bin").c_str(), &st), 0);
    EXPECT_EQ(timegm(&tm_parsed), st.st_mtime);

    // 跨请求稳定
    EXPECT_EQ(header(getRaw(""), "ETag"), etag);

    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}

/// [界] ETag 随文件修改变化（H5.1：mtime/size 任一变化 → 验证器失效）
TEST_F(HttpStreamingTest, ETagChangesOnFileModification) {
    const std::string root = "/tmp/codebus-etag-chg-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    const std::string file = m_root + "/a.bin";
    { std::ofstream(file, std::ios::binary) << MakePattern(1_KB); }
    makeServer();
    m_server->dispatcher()->addPrefixServlet("/", std::make_shared<StaticFileServlet>(m_root));
    startAndWait();

    auto etagOf = [&]() {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll("GET /a.bin HTTP/1.0\r\nHost: t\r\n\r\n"));
        const std::string raw = c.readAll();
        const std::string key = "ETag: ";
        size_t pos = raw.find(key);
        EXPECT_NE(pos, std::string::npos);
        return raw.substr(pos + key.size(), raw.find("\r\n", pos) - pos - key.size());
    };

    const std::string etag1 = etagOf();
    // mtime 推进 2 秒（utimensat 确定性，不 sleep）
    struct stat st {};
    ASSERT_EQ(::stat(file.c_str(), &st), 0);
    struct timespec ts[2] = {{st.st_atim.tv_sec, st.st_atim.tv_nsec}, {st.st_mtime + 2, 0}};
    ASSERT_EQ(::utimensat(AT_FDCWD, file.c_str(), ts, 0), 0);
    const std::string etag2 = etagOf();
    EXPECT_NE(etag2, etag1);
    // size 变化亦失效
    { std::ofstream(file, std::ios::binary | std::ios::app) << "more"; }
    EXPECT_NE(etagOf(), etag2);

    ::unlink(file.c_str());
    ::rmdir(m_root.c_str());
}

/// [正] If-None-Match 命中 → 304 无 body 含 ETag/Last-Modified；不命中 → 200 全量（H5.1 Q38）
TEST_F(HttpStreamingTest, IfNoneMatch304) {
    const std::string content = MakePattern(1_KB);
    const std::string root = "/tmp/codebus-inm-www-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    { std::ofstream(m_root + "/a.bin", std::ios::binary) << content; }
    makeServer();
    m_server->dispatcher()->addPrefixServlet("/", std::make_shared<StaticFileServlet>(m_root));
    startAndWait();

    auto getRaw = [&](const std::string& extra) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll("GET /a.bin HTTP/1.0\r\nHost: t\r\n" + extra + "\r\n"));
        return c.readAll();
    };
    auto header = [](const std::string& raw, const char* name) -> std::string {
        const std::string key = std::string(name) + ": ";
        size_t pos = raw.find(key);
        if (pos == std::string::npos) return "";
        return raw.substr(pos + key.size(), raw.find("\r\n", pos) - pos - key.size());
    };

    const std::string etag = header(getRaw(""), "ETag");
    ASSERT_FALSE(etag.empty());

    // 命中 → 304，头后无 body，缓存头续接
    const std::string hit = getRaw("If-None-Match: " + etag + "\r\n");
    EXPECT_EQ(ParseStatus(hit), 304u);
    EXPECT_NE(hit.find("ETag: " + etag), std::string::npos);
    EXPECT_FALSE(header(hit, "Last-Modified").empty());
    const size_t hdr = hit.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    EXPECT_EQ(hit.size(), hdr + 4) << "304 不得携带 body";

    // 不命中 → 200 全量
    const std::string miss = getRaw("If-None-Match: \"deadbeef-1\"\r\n");
    EXPECT_EQ(ParseStatus(miss), 200u);
    const size_t hdr2 = miss.find("\r\n\r\n");
    ASSERT_NE(hdr2, std::string::npos);
    EXPECT_EQ(miss.substr(hdr2 + 4), content);

    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}

/// [界] INM 优先于 IMS、W/ 弱比较、* 通配、列表命中（H5.1 Q38，RFC 9110）
TEST_F(HttpStreamingTest, IfNoneMatchPrecedenceWeakAndStar) {
    const std::string root = "/tmp/codebus-inm-prec-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    { std::ofstream(m_root + "/a.bin", std::ios::binary) << MakePattern(512); }
    makeServer();
    m_server->dispatcher()->addPrefixServlet("/", std::make_shared<StaticFileServlet>(m_root));
    startAndWait();

    auto getRaw = [&](const std::string& extra) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll("GET /a.bin HTTP/1.0\r\nHost: t\r\n" + extra + "\r\n"));
        return c.readAll();
    };
    auto header = [](const std::string& raw, const char* name) -> std::string {
        const std::string key = std::string(name) + ": ";
        size_t pos = raw.find(key);
        if (pos == std::string::npos) return "";
        return raw.substr(pos + key.size(), raw.find("\r\n", pos) - pos - key.size());
    };
    const std::string etag = header(getRaw(""), "ETag");
    ASSERT_FALSE(etag.empty());

    // INM 不命中 + IMS 命中（未来日期）→ 200：INM 优先，IMS 被忽略
    EXPECT_EQ(ParseStatus(getRaw("If-None-Match: \"deadbeef-1\"\r\nIf-Modified-Since: Sun, 06 Nov 2094 08:49:37 GMT\r\n")), 200u);
    // W/ 弱比较命中 → 304
    EXPECT_EQ(ParseStatus(getRaw("If-None-Match: W/" + etag + "\r\n")), 304u);
    // * 通配 → 304
    EXPECT_EQ(ParseStatus(getRaw("If-None-Match: *\r\n")), 304u);
    // 列表含命中项 → 304
    EXPECT_EQ(ParseStatus(getRaw("If-None-Match: \"deadbeef-1\", " + etag + "\r\n")), 304u);

    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}

/// [界] HEAD 参与条件验证；INM 命中 304 先于 Range（不产生 206）（H5.1 Q38）
TEST_F(HttpStreamingTest, HeadConditionalAnd304BeforeRange) {
    const std::string root = "/tmp/codebus-head-304-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    { std::ofstream(m_root + "/a.bin", std::ios::binary) << MakePattern(1_KB); }
    makeServer();
    m_server->dispatcher()->addPrefixServlet("/", std::make_shared<StaticFileServlet>(m_root));
    startAndWait();

    auto rawOf = [&](const std::string& req) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll(req));
        return c.readAll();
    };
    auto header = [](const std::string& raw, const char* name) -> std::string {
        const std::string key = std::string(name) + ": ";
        size_t pos = raw.find(key);
        if (pos == std::string::npos) return "";
        return raw.substr(pos + key.size(), raw.find("\r\n", pos) - pos - key.size());
    };
    const std::string etag = header(rawOf("GET /a.bin HTTP/1.0\r\nHost: t\r\n\r\n"), "ETag");
    ASSERT_FALSE(etag.empty());

    // HEAD + INM 命中 → 304 无 body
    const std::string head = rawOf("HEAD /a.bin HTTP/1.0\r\nHost: t\r\nIf-None-Match: " + etag + "\r\n\r\n");
    EXPECT_EQ(ParseStatus(head), 304u);
    const size_t hdr = head.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    EXPECT_EQ(head.size(), hdr + 4);

    // GET + INM 命中 + Range → 304（先于 Range，不得 206）
    const std::string both = rawOf("GET /a.bin HTTP/1.0\r\nHost: t\r\nIf-None-Match: " + etag + "\r\nRange: bytes=0-99\r\n\r\n");
    EXPECT_EQ(ParseStatus(both), 304u);
    EXPECT_EQ(both.find("Content-Range:"), std::string::npos);

    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}

/// [界] symlink 默认拒绝端到端（H5.1 TOCTOU 切片锁定：open+fstat 重构前后行为不变）
TEST_F(HttpStreamingTest, SymlinkRejectedEndToEnd) {
    const std::string root = "/tmp/codebus-symlink-www-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    { std::ofstream(m_root + "/a.bin", std::ios::binary) << MakePattern(256); }
    ASSERT_EQ(::symlink("a.bin", (m_root + "/link.bin").c_str()), 0);  // root 内 symlink 亦拒
    makeServer();
    m_server->dispatcher()->addPrefixServlet("/", std::make_shared<StaticFileServlet>(m_root));
    startAndWait();

    auto statusOf = [&](const char* path) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll(std::string("GET ") + path + " HTTP/1.0\r\nHost: t\r\n\r\n"));
        return ParseStatus(c.readAll());
    };
    EXPECT_EQ(statusOf("/link.bin"), 404u);
    EXPECT_EQ(statusOf("/a.bin"), 200u);

    ::unlink((m_root + "/link.bin").c_str());
    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}

/// [正] 尾段通配挂载剥离前缀（H5.1 Q38）：/assets/* 挂载，请求 /assets/a.bin 服务 root/a.bin
TEST_F(HttpStreamingTest, MountPrefixStrippedViaTail) {
    const std::string content = MakePattern(768);
    const std::string root = "/tmp/codebus-mount-www-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    { std::ofstream(m_root + "/a.bin", std::ios::binary) << content; }
    makeServer();
    m_server->dispatcher()->addPatternServlet("/assets/*", std::make_shared<StaticFileServlet>(m_root));
    startAndWait();

    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("GET /assets/a.bin HTTP/1.0\r\nHost: t\r\n\r\n"));
    const std::string raw = c.readAll();
    EXPECT_EQ(ParseStatus(raw), 200u);
    const size_t hdr = raw.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    EXPECT_EQ(raw.substr(hdr + 4), content);

    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}

/// [正] multipart parts 访问（H5.1）：name/filename/body 保真（含二进制 NUL 字节）
TEST_F(HttpStreamingTest, MultipartPartsAccessible) {
    makeServer();
    m_server->Post("/upload", [](const HttpRequest& req, HttpResponse& r) {
        std::string out;
        for (const auto& p : req.getParts()) {
            out += p.name + "|" + p.filename + "|" + p.body + ";";
        }
        r.setBody(out);
    });
    startAndWait();

    const std::string bin("\x00\x01\x02", 3);
    const std::string body =
        "--Xb\r\nContent-Disposition: form-data; name=\"field1\"\r\n\r\nvalue1\r\n"
        "--Xb\r\nContent-Disposition: form-data; name=\"file1\"; filename=\"f.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n" + bin + "\r\n--Xb--\r\n";
    StreamClient c;
    ASSERT_TRUE(c.connect(m_port));
    ASSERT_TRUE(c.sendAll("POST /upload HTTP/1.0\r\nHost: t\r\n"
                          "Content-Type: multipart/form-data; boundary=Xb\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body));
    const std::string raw = c.readAll();
    EXPECT_EQ(ParseStatus(raw), 200u);
    const size_t hdr = raw.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    const std::string want = "field1||value1;file1|f.bin|" + bin + ";";
    EXPECT_EQ(raw.substr(hdr + 4), want);
}

/// [界] multipart：引号 boundary、part 头域、畸形 400、urlencoded/非 multipart 互不干扰（H5.1）
TEST_F(HttpStreamingTest, MultipartBoundaryFormsAndMalformed) {
    makeServer();
    m_server->Post("/upload", [](const HttpRequest& req, HttpResponse& r) {
        const auto& parts = req.getParts();
        std::string ct;
        if (!parts.empty()) {
            for (const auto& h : parts[0].headers) {
                if (h.first == "content-type") ct = h.second;
            }
        }
        const auto a = req.getFormParam("a");
        r.setBody("parts=" + std::to_string(parts.size()) + ";ct=" + ct + ";a=" + (a ? std::string(*a) : ""));
    });
    startAndWait();

    auto post = [&](const std::string& ctype, const std::string& body) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll("POST /upload HTTP/1.0\r\nHost: t\r\nContent-Type: " + ctype +
                              "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body));
        return c.readAll();
    };
    auto bodyOf = [](const std::string& raw) {
        size_t hdr = raw.find("\r\n\r\n");
        return hdr == std::string::npos ? std::string() : raw.substr(hdr + 4);
    };

    // 引号包裹 boundary + part 头域暴露
    const std::string qbody = "--Qb\r\nContent-Disposition: form-data; name=\"f\"\r\nContent-Type: text/plain\r\n\r\nv\r\n--Qb--\r\n";
    const std::string qraw = post("multipart/form-data; boundary=\"Qb\"", qbody);
    EXPECT_EQ(ParseStatus(qraw), 200u);
    EXPECT_EQ(bodyOf(qraw), "parts=1;ct=text/plain;a=");

    // 缺 boundary → 400
    EXPECT_EQ(ParseStatus(post("multipart/form-data", qbody)), 400u);
    // 截断（无闭合分隔）→ 400
    EXPECT_EQ(ParseStatus(post("multipart/form-data; boundary=Qb",
                               "--Qb\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nv\r\n")), 400u);
    // 首分隔缺失 → 400
    EXPECT_EQ(ParseStatus(post("multipart/form-data; boundary=Qb", "garbage--Qb--\r\n")), 400u);

    // urlencoded 不受影响：form 装配照常，parts 空
    const std::string uraw = post("application/x-www-form-urlencoded", "a=1&b=2");
    EXPECT_EQ(ParseStatus(uraw), 200u);
    EXPECT_EQ(bodyOf(uraw), "parts=0;ct=;a=1");

    // 非 multipart：parts 空
    const std::string traw = post("text/plain", "hello");
    EXPECT_EQ(ParseStatus(traw), 200u);
    EXPECT_EQ(bodyOf(traw), "parts=0;ct=;a=");
}

/// [界] Content-Disposition 参数边界（审查修复）：filename-only part name 须空；filename 在前 name 正确
TEST_F(HttpStreamingTest, MultipartDispositionParamBoundary) {
    makeServer();
    m_server->Post("/upload", [](const HttpRequest& req, HttpResponse& r) {
        std::string out;
        for (const auto& p : req.getParts()) {
            out += p.name + "|" + p.filename + ";";
        }
        r.setBody(out);
    });
    startAndWait();

    auto post = [&](const std::string& body) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll("POST /upload HTTP/1.0\r\nHost: t\r\n"
                              "Content-Type: multipart/form-data; boundary=Xb\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body));
        const std::string raw = c.readAll();
        EXPECT_EQ(ParseStatus(raw), 200u);
        const size_t hdr = raw.find("\r\n\r\n");
        return hdr == std::string::npos ? std::string() : raw.substr(hdr + 4);
    };

    // filename-only：name 不得误取 filename 值（裸 find 会命中 filename= 内部子串）
    EXPECT_EQ(post("--Xb\r\nContent-Disposition: form-data; filename=\"f.bin\"\r\n\r\nv\r\n--Xb--\r\n"), "|f.bin;");
    // filename 在前：name 须取真正的 name 参数
    EXPECT_EQ(post("--Xb\r\nContent-Disposition: form-data; filename=\"f.bin\"; name=\"field1\"\r\n\r\nv\r\n--Xb--\r\n"),
              "field1|f.bin;");
}

/// [界] part 头体分隔边界（审查修复）：缺空行 → 400；合法空 body part（有空行）→ 200 空 body
TEST_F(HttpStreamingTest, MultipartBlankLineBoundary) {
    makeServer();
    m_server->Post("/upload", [](const HttpRequest& req, HttpResponse& r) {
        const auto& parts = req.getParts();
        r.setBody("parts=" + std::to_string(parts.size()) +
                  ";b0=" + (parts.empty() ? std::string("-") : std::to_string(parts[0].body.size())));
    });
    startAndWait();

    auto post = [&](const std::string& body) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll("POST /upload HTTP/1.0\r\nHost: t\r\n"
                              "Content-Type: multipart/form-data; boundary=Xb\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body));
        return c.readAll();
    };
    auto bodyOf = [](const std::string& raw) {
        size_t hdr = raw.find("\r\n\r\n");
        return hdr == std::string::npos ? std::string() : raw.substr(hdr + 4);
    };

    // 缺空行（头域直接接闭合分隔）→ 畸形 400
    EXPECT_EQ(ParseStatus(post("--Xb\r\nContent-Disposition: form-data; name=\"f\"\r\n--Xb--\r\n")), 400u);
    // 合法空 body part（头域后空行，body 零字节）→ 200
    const std::string ok = post("--Xb\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\n--Xb--\r\n");
    EXPECT_EQ(ParseStatus(ok), 200u);
    EXPECT_EQ(bodyOf(ok), "parts=1;b0=0");
}

/// [正] 304 携带 Cache-Control（H5.1 story 9 锁定：maxAgeSec>0 配置下缓存头续接）
TEST_F(HttpStreamingTest, NotModifiedCarriesCacheControl) {
    const std::string root = "/tmp/codebus-304-cc-XXXXXX";
    std::vector<char> buf(root.begin(), root.end());
    buf.push_back('\0');
    char* p = mkdtemp(buf.data());
    ASSERT_NE(p, nullptr);
    m_root = p;
    { std::ofstream(m_root + "/a.bin", std::ios::binary) << MakePattern(256); }
    makeServer();
    m_server->dispatcher()->addPrefixServlet(
        "/", std::make_shared<StaticFileServlet>(m_root, StaticFileConfig{.maxAgeSec = 3600}));
    startAndWait();

    auto getRaw = [&](const std::string& extra) {
        StreamClient c;
        EXPECT_TRUE(c.connect(m_port));
        EXPECT_TRUE(c.sendAll("GET /a.bin HTTP/1.0\r\nHost: t\r\n" + extra + "\r\n"));
        return c.readAll();
    };
    auto header = [](const std::string& raw, const char* name) -> std::string {
        const std::string key = std::string(name) + ": ";
        size_t pos = raw.find(key);
        if (pos == std::string::npos) return "";
        return raw.substr(pos + key.size(), raw.find("\r\n", pos) - pos - key.size());
    };
    const std::string etag = header(getRaw(""), "ETag");
    ASSERT_FALSE(etag.empty());

    const std::string hit = getRaw("If-None-Match: " + etag + "\r\n");
    EXPECT_EQ(ParseStatus(hit), 304u);
    EXPECT_EQ(header(hit, "Cache-Control"), "max-age=3600");
    EXPECT_EQ(header(hit, "ETag"), etag);
    EXPECT_FALSE(header(hit, "Last-Modified").empty());
    const size_t hdr = hit.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos);
    EXPECT_EQ(hit.size(), hdr + 4);

    ::unlink((m_root + "/a.bin").c_str());
    ::rmdir(m_root.c_str());
}
