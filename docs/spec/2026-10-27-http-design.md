# HTTP 模块（Server 侧）设计与计划（全栈分单元 · 交互式）

> 状态：已实施完成（V0–V6 全部落地，2026-10-27）
> 关联：[TCP Server 设计](2026-10-22-tcp-server-design.md) · [UDP 设计](2026-10-24-udp-design.md)

## 1. 需求背景

项目首个协议层模块：在 `src/net/http/`（`namespace bus::http`）实现 HTTP/1.1 Server，
解析层复用已引入未接线的 `3rd/http_parser`（v2.9.4+ joyent 归档版，提交 4b0ab5d，仅 .c/.h 两文件，
extern "C" 守卫完备）。

**架构与接口借鉴 Java Web 生态事实标准**（javax.servlet 规范 + Spring MVC DispatcherServlet
心智模型），亦是 sylar http 模块的原型；C++ 化保留编程模型亲切性，底层贴合 codebus
one-loop-per-thread 事件驱动架构。

### Java 构件映射与取舍

| Java 构件 | codebus 对应物 | 取舍说明 |
|---|---|---|
| Connector | `HttpServer`（组合 `TcpServer`） | 组合不继承：TcpServer 生命周期契约原样透传 |
| HttpServletRequest | `HttpRequest` | getXxx 只读视图；装配器供 parser；query/Cookie 预解析 |
| HttpServletResponse | `HttpResponse` | setXxx/addCookie/SendError/SendRedirect；`Dump(req)` 统一序列化 |
| HttpServlet | `Servlet` + `HttpServlet` | `Handle`（对应 service）+ doGet/doPost 虚函数分派，未覆盖 405+Allow |
| DispatcherServlet | `ServletDispatcher`（本身是 Servlet，可嵌套） | 精确表 O(1) + 最长前缀表 |
| Filter/FilterChain | `Filter`/`FilterChain` | 全局链、`chain.doFilter` 放行/短路 |
| init()/destroy() | 不做 | C++ 生命周期即 shared_ptr 构造/析构 |
| ServletContext/Listener | 延后登记 | YAGNI |
| HttpSession(JSESSIONID) | 延后（L6） | Cookie 层本轮就位 |
| Tomcat worker 池 | `HttpServerConfig::workerNum` | 0=IO 线程内联（默认）；>0=投递 ThreadPool |

接口命名：`doGet/doPost/doPut/doDelete/doHead/doPatch/doOptions/doFilter` 保持 Java 驼峰原名；
其余遵循 `.claude/rules/code_style.md`。

## 2. 已裁决设计决策（用户确认 2026-10-27）

| # | 决策点 | 裁决 |
|---|---|---|
| 1 | 交付范围 | **仅 HTTP Server**；HTTP Client 延后独立轮（复用本轮 parser 封装与协议对象） |
| 2 | Servlet 模型 | **Java 继承式**：`Servlet::Handle()` 抽象 + `HttpServlet` 方法分发 + Dispatcher 精确/最长前缀路由 |
| 3 | 线程模型 | **可配置业务池**：`workerNum`；0=IO 线程内联（默认），>0=解析在 IO 线程、Servlet 投递 ThreadPool；pipeline 同连接响应按序（单飞规则） |
| 4 | Filter 链 | **本轮纳入**：`Filter::doFilter(req, resp, chain)` 全局链 + `HttpServer::addFilter` |

我职责内定的实现细节（已在方案审阅中呈现）：命名 `ServletDispatcher`（非 sylar "Dispatchor"
拼写）；会话状态藏 HttpServer.cc + mutex map（conn 回调分散各 ioLoop 无汇合点）；keep-alive/慢速
防护用 base loop 中心扫描（TimerManager loop 线程构造/析构纪律与 TcpServer 无 per-loop teardown
钩子冲突，不做 per-conn timer）；`HttpServerConfig` 聚合体第 4 默认参；枚举用 3rd 宏生成（与 C 枚举
数值恒等，static_cast 零成本）；Servlet init/destroy 不做；URL decode `%XX` 全支持、`+`→空格仅
query、非法 `%` 宽容保留、path 命中 `%00` 整请求 400、`%2F` 登记已知限制。

## 3. 复用的既有资产与纪律

- **TcpServer/TCPConnection**（src/net/）：`MessageCallback = void(TCPConnection::ptr, Buffer*)`，
  消费后 `SkipRead`；send 四重载全跨线程安全（非 loop 线程拷贝入队）；shutdown/close 亦跨线程
  安全；TCPConnection **无 context**（HTTP 层自管会话）。
- **ThreadPool**（src/thread/）：`run/start/stopAndDrain`——worker 模式与 Stop 排空复用。
- **TimerManager**（src/Timer.h）：loop 线程构造/析构纪律；TcpClient 惰性 `unique_ptr` 先例。
- **D39/D40/C1 纪律**：会析构对象的 Stop 必须 QueueInLoop；关闭连接一律 `shutdown()`（D40 安全
  路径），5s 滞留兜底 `close()`。
- **http_parser 契约**：data 回调任意分片（必须即时 append 累加器）；`execute` 返回值<len 三态
  （errno/upgrade/paused）；无 reinit（复用=重调 init）；`content_length` 无 CL 头时
  `(uint64_t)-1`；`HTTP_MAX_HEADER_SIZE` 80KB 内建（`http_parser_set_max_header_size` 是进程级
  全局，不调用）；CONNECT/Upgrade 头触发 upgrade 态。
- **测试基建**：`codebus_add_test`、LoopThread/GetFreePort/WaitFor、三矩阵（常规/TSAN
  die_after_fork=0+setarch -R/ASAN）、变体切换 `rm bin/*Test lib/libcodebus.so` 强制重链。

## 4. 分层架构与文件清单

```
L5 Filter 链       ← javax.servlet.Filter/FilterChain（全局链，Servlet 前）
L4 Servlet 容器    ← HttpServlet + ServletDispatcher(HandlerMapping) + NotFound/405/500 兜底
L3 HttpServer      ← Tomcat Connector+Container（组合 codebus TcpServer）
L2 解析适配        ← HttpRequestParser（封装 http_parser 增量回调→完整请求）
                   ← HttpResponse::Dump(req) 序列化
L1 协议对象        ← HttpRequest/HttpResponse/HeaderMap/Cookie/Method/Status/HttpUtil
L0 3rd/http_parser ← C 静态库（仿 tomlc17 接入）
```

**数据流**：`conn → Buffer* → parser.Execute() → HttpRequest → Filter 链 → Dispatcher →
Servlet::Handle → doGet/doPost → HttpResponse → Dump(req) → conn->send()`

```
src/net/http/（8 对，依赖单向向下）
├── HttpProtocol.h            仅头：Method/Status 枚举（3rd 宏生成）+ ToString
├── HttpUtil.h / .cc          纯函数：URL 编解码/ParseQuery/EqualCI/Cookie 解析与序列化
├── HttpMessage.h / .cc       HeaderMap/Cookie/HttpRequest/HttpResponse（含 Dump）
├── HttpRequestParser.h / .cc ParseEvent/ParseResult/HttpParseError/HttpRequestParser
├── Filter.h / .cc            Filter/FilterChain
├── Servlet.h / .cc           Servlet/HttpServlet/FunctionServlet/NotFoundServlet
├── ServletDispatcher.h / .cc ServletDispatcher（精确表+前缀表+Filter 链驱动）
└── HttpServer.h / .cc        HttpServer/HttpServerConfig（.cc 内私有 HttpSession）
```

CMake 三处：`3rd/CMakeLists.txt` 加 http_parser STATIC（仿 tomlc17）；`src/CMakeLists.txt` 源列表
+7 .cc 并链接；`test/CMakeLists.txt` 新增 `net/http/` 段。

**接口签名级定义**：见本文末附录（与已批准计划一致）。

## 5. 处理单元设计（V0–V6）

| 单元 | 内容 | 测试目标（test/net/http/）关键用例 |
|---|---|---|
| V0 | CMake 接线 + HttpProtocol.h | HttpProtocolTest：枚举与 C API static_cast 恒等/ToString 正反例 |
| V1 | HttpUtil | HttpUtilTest：编解码正例（中文/空格/`+`）；边界（`%2F`/截断 `%G`/`%00`/空段/无 `=`）；恶意（超长/`%%`/NUL） |
| V2 | HttpMessage（HeaderMap/Cookie/Req/Resp/Dump） | HttpMessageTest：大小写命中/覆盖/多值；Dump 快照（CL 自动/HEAD 剥 body/204 无 body/Set-Cookie 多行/Connection 两态）；SendError/SendRedirect |
| V3 | HttpRequestParser 全状态机 | HttpRequestParserTest（纯函数无网络）：单字节逐喂分片拼接/pipeline consumed 精确/chunked/无 CL GET/413/431/恶意矩阵（坏版本/坏方法/双 CL/CL+chunked/坏 chunk size/upgrade/CONNECT）/复用 100 次 |
| V4 | Servlet 容器 + Filter 链 | ServletDispatcherTest：精确>前缀/最长前缀/404/405+Allow/doHead 复用 doGet/异常 500；Filter 顺序/短路（不放行 403 且 Servlet 未执行）/Filter 异常 500 |
| V5 | HttpServer 传输（workerNum=0） | HttpServerTest（真 socket）：200/404/keep-alive 两请求/Connection:close/pipeline/HEAD CL 正确无 body/413/431/400/慢头 408/空闲超时关/100-continue/Start-Stop 幂等/handler 内 Stop（D39 同型）/32 并发 |
| V6 | worker 池 + 收尾 | HttpServerWorkerTest：handler sleep 保序/单飞 pendingInput 回灌不假死（定向）/Stop 在途任务 stopAndDrain 不死锁；Date 头；docs 收尾 |

顺序即依赖序：V1→V3 纯函数层先行，V4 容器无网络，V5/V6 才动传输。

## 6. 交互式处理协议（每单元一轮）

1. 代码确认：呈 before/after（新文件全文/关键实现段）待用户确认
2. TDD：测试用例先行（编译红）→ CMake 注册新测试目标
3. 实现 → 编译绿
4. 验证：`ctest -R Http` + 全量回归；V5/V6 加 TSAN/ASAN 矩阵
5. 登记：本文进度记录表滚动更新；缺陷按 D42/Q33 起登记 known-issues.md

## 7. 验证策略

| 时机 | 动作 |
|---|---|
| 每单元 | `ctest --test-dir build -R Http --output-on-failure` + 全量回归 |
| V3 后 | 纯函数层 ASAN 抽验（Http* 全目标） |
| V5/V6 后 | TSAN（`TSAN_OPTIONS=die_after_fork=0 setarch $(uname -m) -R`）+ ASAN 全矩阵 |
| 终验 | N×{常规, TSAN, ASAN} + `--repeat until-fail:10`（HttpServer/Worker/Paser 三目标） |
| 每次变体切换 | `rm bin/*Test lib/libcodebus.so && cmake --build <dir>` 强制重链 |

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| http_parser 停维（joyent 归档） | 源码钉死入树；C API 仅出现在 HttpRequestParser 一处，未来可换 llhttp 只动一个类 |
| worker 模式 pipeline 乱序/假死 | 单飞 + pendingInput 回灌（不能留 TCPConnection 输入 Buffer 等下次 EPOLLIN——用户态数据无新事件）；V6 定向用例 |
| 慢速攻击（slowloris） | 头时限 408 扫描关闭；慢 body 不限时（总量封顶兜底，登记） |
| 大 body 内存放大 | CL 前置零字节拒收 + maxBodyBytes 可配；流式登记后续 |
| 会话 map 跨 IO 线程 | mutex 仅护增删查、会话内原子量；TSAN 矩阵验证 |
| Expect: 100-continue 不处理致 curl hang | headers_complete 即回 100（必做项） |
| 路由注册竞态 | 注册仅 Start 前，热路径零锁 |

## 9. 进度记录

| 单元 | 状态 | 备注 |
|---|---|---|
| V0 CMake 接线 + HttpProtocol | **完成**（2026-10-27） | TDD 红→绿；43/43 全绿。http_parser 以 STATIC+PUBLIC include 接入（仿 tomlc17）；非法值哨兵实测为 `"<unknown>"`（小写，ELEM_AT 边界检查+switch default） |
| V1 HttpUtil | **完成**（2026-10-27） | TDD 红→绿；44/44 全绿。范围微调：ParseRequestCookies/SerializeSetCookie 挪 V2（依赖 Cookie 结构，随结构内聚放 HttpMessage） |
| V2 HttpMessage | **完成**（2026-10-27） | TDD 红→绿；45/45 全绿。实施修正：HeaderMap 增 Swap 成员（Swap 链路）；Cookie 值不编码（调用方保证，文档注明）
| V3 HttpRequestParser | **完成**（2026-10-27） | TDD 红→绿；46/46 全绿 + ASAN Http 四目标绿。实施修正：①onUrl 漏调 appendUrl（getRawUrl 空，测试抓住）②"错误优先判定序"经测试验证必要（CL 超限伴生 message_complete）③HTTP/9.9 实为合法 major=9，坏版本用例改 HTTP/12.3 ④pipeline 须循环喂（pause 语义）在用例中固化
| V4 Servlet 容器 + Filter | **完成**（2026-10-27） | TDD 红→绿；47/47 全绿。实施增补：HttpResponse::getHeader 读接口（测试红面抓住缺口，Java 亦有）；前缀段边界判定（/api 不匹配 /apiv2）落地
| V5 HttpServer 传输 | **完成**（2026-10-27） | TDD 红→绿；48/48 全绿 + TSAN/ASAN 双绿。实施修正：①**session 自环泄漏**（ASAN 抓获）：parser 回调 lambda 捕 session 的 shared_ptr→改裸指针（parser 属 session，回调期必存活）②测试 release() 规避重复 Stop→HttpServer 永不析构，改 m_stop_done 标志③编译期四小修：TimerManager::ClockType 类内别名/kURI_TOO_LONG 实际枚举名/EventLoop include/conn->name() 返回 string_view 需包 std::string
| V6 worker 池 + 收尾 | **完成**（2026-10-27） | TDD 行为红（假死 90s/阻塞传染/Date 缺失三症状精确复现）→绿；常规 49/49 + TSAN/ASAN Http 全目标绿。实施要点：onMessage 循环抽取 processInput（Buffer 驱动与 pendingInput 回灌共用）；单飞回灌防假死；StopDrainsInflightWorkerTask 锁死 stopAndDrain 语义；Date 头（RFC1123 GMT，date 列入托管头）；CLAUDE.md 模块表述/49 目标/待实现清单同步 |

**轮次收口（2026-10-27）**：V0–V6 全部完成。常规 49/49；TSAN/ASAN `Http*` 全目标绿（ASAN 零泄漏）。累计 7 个测试目标（+42 基线）、8 对源文件、CMake 三处接线。

## 附录：接口签名级定义

（与已批准实施计划一致，实现以此为准）

### A.1 HttpProtocol.h

```cpp
namespace bus::http
{
// 用 3rd 的 HTTP_METHOD_MAP/HTTP_STATUS_MAP 宏生成枚举——数值与 C 枚举恒等，
// static_cast 零成本互转，免维护 34+61 条映射
enum class HttpMethod : uint8_t  { HTTP_METHOD_MAP(XX) };   // kGET=1, kPOST=3, ...
enum class HttpStatus  : uint16_t { HTTP_STATUS_MAP(XX) };  // kOK=200, kNotFound=404, ...

ATTR_NODISCARD std::string_view HttpMethodToString(HttpMethod) noexcept;   // "GET"；未知 "<UNKNOWN>"
ATTR_NODISCARD std::string_view HttpStatusToString(HttpStatus) noexcept;   // "OK"/"Not Found"
} // namespace bus::http
```

### A.2 HttpUtil（自由函数，平铺 bus::http）

```cpp
ATTR_NODISCARD std::string UrlDecode(std::string_view in, bool plusAsSpace) noexcept;
ATTR_NODISCARD std::string UrlEncode(std::string_view in) noexcept;
ATTR_NODISCARD bool ParseQuery(std::string_view raw,
                               std::vector<std::pair<std::string, std::string>>& out) noexcept;
ATTR_NODISCARD bool EqualCI(std::string_view a, std::string_view b) noexcept;  // ASCII fold
ATTR_NODISCARD std::vector<Cookie> ParseRequestCookies(std::string_view header) noexcept;
ATTR_NODISCARD std::string SerializeSetCookie(const Cookie& c) noexcept;
```

URL decode 裁决：`%XX` 全支持；`+`→空格仅 query；非法 `%` 宽容保留字面（DEBUG 日志）；`%00`
query 保留、path 命中整请求 400；`%2F` 已知限制（V2 可加拒绝开关）。

### A.3 HeaderMap / Cookie

```cpp
/// @brief HTTP 头集：原序 vector + ASCII 大小写不敏感查找（典型 n≤40，缓存友好天然保序）
class HeaderMap : public copyable
{
 public:
    ATTR_NODISCARD std::optional<std::string_view> getHeader(std::string_view name) const noexcept;
    ATTR_NODISCARD std::vector<std::string_view> getHeaders(std::string_view name) const;
    void setHeader(std::string_view name, std::string_view value);   // 覆盖同名
    void addHeader(std::string_view name, std::string_view value);   // 允许重复头
    ATTR_NODISCARD bool hasHeader(std::string_view name) const noexcept;
    ATTR_NODISCARD size_t size() const noexcept;
    ATTR_NODISCARD auto begin() const noexcept;  ATTR_NODISCARD auto end() const noexcept;
    void clear() noexcept;
};

struct Cookie : public copyable
{
    std::string name, value;
    std::string path = "/", domain, expires;   // 空 = 不输出该属性
    int64_t maxAge = -1;                        // <0 = 不输出
    bool secure = false, httpOnly = false;
    std::string sameSite;                       // 空 = 不输出
};
```

### A.4 HttpRequest

```cpp
class HttpRequest : public copyable
{
 public:
    // 请求行（parser 装配；业务只读）
    ATTR_PURE_INLINE HttpMethod getMethod() const noexcept;
    ATTR_PURE_INLINE uint16_t getHttpMajor() const noexcept;
    ATTR_PURE_INLINE uint16_t getHttpMinor() const noexcept;
    ATTR_NODISCARD std::string getVersion() const noexcept;            // "HTTP/1.1"
    ATTR_NODISCARD const std::string& getRawUrl() const noexcept;
    ATTR_NODISCARD const std::string& getPath() const noexcept;        // %XX 已解码，'+' 不解码
    ATTR_NODISCARD const std::string& getQuery() const noexcept;
    // 参数/头/Cookie（query 已预解码）
    ATTR_NODISCARD std::optional<std::string_view> getParam(std::string_view key) const;
    ATTR_NODISCARD std::vector<std::string_view> getParams(std::string_view key) const;
    ATTR_NODISCARD std::optional<std::string_view> getHeader(std::string_view name) const;
    ATTR_NODISCARD std::span<const Cookie> getCookies() const noexcept;
    ATTR_NODISCARD std::optional<std::string_view> getCookie(std::string_view name) const;
    // body / 连接语义
    ATTR_NODISCARD std::string_view getBody() const noexcept;
    ATTR_PURE_INLINE size_t getContentLength() const noexcept;
    ATTR_NODISCARD bool isKeepAlive() const noexcept;
    ATTR_NODISCARD bool isChunked() const noexcept;
    ATTR_NODISCARD bool isExpectContinue() const noexcept;
    void Swap(HttpRequest& o) noexcept;
    // 装配器（public 但 Doxygen 标注"仅供 parser 与测试"）
    // setMethod/setMajor/setMinor/appendUrl/setPath/setQuery/addHeaderRaw/appendBody/
    // setKeepAlive/setChunked/setExpectContinue/setContentLength/finishQuery/finishCookies
};
```

### A.5 HttpResponse

```cpp
class HttpResponse : public copyable
{
 public:
    explicit HttpResponse(std::string serverName = "codebus-http");
    ATTR_PURE_INLINE HttpStatus getStatus() const noexcept;   void setStatus(HttpStatus) noexcept;
    void setHeader(std::string_view name, std::string_view value);
    void addHeader(std::string_view name, std::string_view value);
    void addCookie(Cookie c);
    void setBody(std::string body) noexcept;
    void setBody(std::string_view body);
    void appendBody(std::string_view chunk);
    ATTR_NODISCARD std::string_view getBody() const noexcept;
    void SendError(HttpStatus s, std::string_view msg = {});
    void SendRedirect(std::string_view location);              // 302
    void setKeepAlive(bool on) noexcept;
    ATTR_NODISCARD bool isKeepAlive() const noexcept;
    ATTR_NODISCARD std::string Dump(const HttpRequest& req) const;
};
```

Dump：`HTTP/1.x <code> <reason>`（版本跟随请求）→ Server → 用户头（插入序）→ Set-Cookie（每条
独立行）→ Content-Length（HEAD 输出应有长度）→ Connection（= `req.isKeepAlive() &&
resp.isKeepAlive()`）→ 空行 → body（HEAD/1xx/204/304 省略）。用户设 Transfer-Encoding → 剥离+WARN。

### A.6 HttpRequestParser

```cpp
enum class ParseEvent { kNeedMore, kComplete, kError, kUpgrade };
enum class HttpParseError { kNone, kBadRequest, kHeaderTooLarge, kBodyTooLarge, kUrlTooLong };
struct ParseResult { ParseEvent event = ParseEvent::kNeedMore;
                     size_t consumed = 0; HttpParseError error = HttpParseError::kNone; };

class HttpRequestParser : noncopyable
{
 public:
    using ExpectContinueCallback = std::function<void ()>;
    using MessageBeginCallback  = std::function<void ()>;

    explicit HttpRequestParser(size_t maxBodyBytes = 8_MB, size_t maxUrlBytes = 8_KB);
    ParseResult Execute(const char* data, size_t len);   // kComplete 后自动复位可续喂
    ATTR_NODISCARD HttpRequest TakeRequest();
    void Reset();
    ATTR_NODISCARD bool isIdle() const noexcept;
    ATTR_PURE_INLINE size_t parsedCount() const noexcept;
    void setOnExpectContinue(ExpectContinueCallback) noexcept;
    void setOnMessageBegin(MessageBeginCallback) noexcept;
};
```

### A.7 Servlet 体系

```cpp
class Servlet
{
 public:
    using ptr = std::shared_ptr<Servlet>;
    virtual ~Servlet() = default;
    virtual void Handle(const HttpRequest& req, HttpResponse& resp) = 0;
    ATTR_NODISCARD virtual std::string_view getName() const noexcept { return "Servlet"; }
};

class HttpServlet : public Servlet
{
 public:
    void Handle(const HttpRequest& req, HttpResponse& resp) override;   // switch(method) 分派
 protected:
    virtual void doGet   (const HttpRequest&, HttpResponse&) { MethodNotAllowed(resp, "GET"); }
    virtual void doPost  (const HttpRequest&, HttpResponse&) { MethodNotAllowed(resp, "POST"); }
    virtual void doPut   (const HttpRequest&, HttpResponse&) { MethodNotAllowed(resp, "PUT"); }
    virtual void doDelete(const HttpRequest&, HttpResponse&) { MethodNotAllowed(resp, "DELETE"); }
    virtual void doPatch (const HttpRequest&, HttpResponse&) { MethodNotAllowed(resp, "PATCH"); }
    virtual void doHead  (const HttpRequest& req, HttpResponse& resp) { doGet(req, resp); }
    virtual void doOptions(const HttpRequest&, HttpResponse&);          // Allow 列表
 private:
    static void MethodNotAllowed(HttpResponse&, std::string_view allow); // 405 + Allow
};

class FunctionServlet : public Servlet
{
 public:
    using Handler = std::function<void (const HttpRequest&, HttpResponse&)>;
    explicit FunctionServlet(Handler h);
    void Handle(const HttpRequest& req, HttpResponse& resp) override;
};

class NotFoundServlet : public Servlet
{ public: void Handle(const HttpRequest&, HttpResponse& resp) override; };
```

### A.8 Filter 链

```cpp
class FilterChain;

class Filter
{
 public:
    using ptr = std::shared_ptr<Filter>;
    virtual ~Filter() = default;
    /// 调用 chain.doFilter 放行到下一环（或末端 Servlet）；不调用即短路
    /// @attention 与 Servlet 同线程执行（IO 线程或 worker 池）
    virtual void doFilter(const HttpRequest& req, HttpResponse& resp, FilterChain& chain) = 0;
};

class FilterChain    // 仅 ServletDispatcher 构造（friend）；Dispatcher 持 shared_ptr 保裸指针安全
{
 public:
    void doFilter(const HttpRequest& req, HttpResponse& resp);
 private:
    friend class ServletDispatcher;
    FilterChain(std::span<Filter* const> filters, Servlet* target);
    std::span<Filter* const> m_filters;
    Servlet* m_target;
    size_t m_index = 0;
};
```

### A.9 ServletDispatcher

```cpp
class ServletDispatcher : public Servlet, noncopyable
{
 public:
    using ptr = std::shared_ptr<ServletDispatcher>;
    void addExactServlet(std::string_view path, Servlet::ptr s);     // 精确优先
    void addPrefixServlet(std::string_view prefix, Servlet::ptr s);  // 最长前缀；同长后注册胜
    ATTR_NODISCARD Servlet::ptr getServlet(const HttpRequest& req) const;
    void Handle(const HttpRequest& req, HttpResponse& resp) override;  // Filter 链→Servlet；异常兜底 500
 private:
    std::unordered_map<std::string, Servlet::ptr> m_exact;
    std::vector<std::pair<std::string, Servlet::ptr>> m_prefix;
    std::vector<Filter::ptr> m_filters;      // Start 前注册，热路径零锁
    NotFoundServlet m_default;
};
```

### A.10 HttpServer

```cpp
struct HttpServerConfig
{
    std::string serverName    = "codebus-http";
    uint32_t keepAliveIdleSec = 75;
    uint32_t headerTimeoutSec = 30;
    size_t   maxBodyBytes     = 8_MB;
    size_t   maxUrlBytes      = 8_KB;
    uint32_t workerNum        = 0;
};

class HttpServer : noncopyable
{
 public:
    using ptr = std::shared_ptr<HttpServer>;
    using DoneCallback = std::function<void ()>;
    using Handler = FunctionServlet::Handler;

    /// @throw Exception::AddressError/SocketError（bind 失败，同 TcpServer）
    HttpServer(EventLoop* loop, std::string_view addr, uint32_t thread_num,
               HttpServerConfig cfg = HttpServerConfig());
    void Start();
    void Stop(DoneCallback cb = DoneCallback());
    void setDispatcher(ServletDispatcher::ptr d);
    ServletDispatcher* dispatcher() noexcept;
    void addFilter(Filter::ptr f);
    void Get/Post/Put/Delete(std::string_view path, Handler h);
    void setConnectionObserver(std::function<void (const TCPConnection::ptr&, bool up)> ob);
};
```

## 关键机制（实现依据）

1. **解析状态机**：分片回调即时 append；header 两段式配对收割；on_headers_complete（恒返回 0，
   CL 超限除外——返回 1 走 F_SKIPBODY 零字节 413）含 parse_url 切分 + Expect 回调；
   on_message_complete 置标志 + `http_parser_pause(1)` 精确停界 → kComplete 后重 init 自复位。
   错误翻译：kBodyTooLarge→413、kUrlTooLong→414、HPE_HEADER_OVERFLOW→431、其余 HPE_*→400、
   upgrade（含 CONNECT）→501；**凡解析错误一律回码 + shutdown() 半关闭**。
2. **会话与单飞**：HttpSession{parser, weak conn, lastActiveNs, requestStartNs, responsePending,
   pendingInput}；worker 模式同连接至多一个在途响应，期间字节入 pendingInput（防连接假死），
   响应落盘 RunInLoop 回灌；IO 模式内联天然保序。
3. **中心扫描**：base loop TimerManager（Start 惰性建/Stop DoneCallback 内析构，D40 同型）；
   interval = max(1, min(idle, headerTimeout)/2)s；扫 408/空闲/半关滞留 5s 兜底 close。
4. **Stop**：透传 TcpServer::Stop（D39 queue-only）→ DoneCallback 内清定时器 + worker 池
   stopAndDrain；~HttpServer 须已 Stop 且 DoneCallback 已触发。
5. **TCP 栈 backlog（本轮不做）**：`TCPConnection::inputBuffer()` 访问器与 `setContext/
   getContext`——落地即删 pendingInput 拷贝与 name→session map 两处绕行。
