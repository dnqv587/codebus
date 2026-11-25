# H5.1 · 服务端静态能力补全（ETag 完整 Q38 + multipart 解析）

Status: ready-for-agent

## Problem Statement

框架使用者在 HTTP 服务端静态能力上遇到两组缺口：

- 静态文件消费者无法做精确缓存验证：响应没有 ETag 与 Last-Modified 头，条件请求只有 If-Modified-Since 一条腿，缓存命中判定粗糙且不符合 RFC 9110 验证优先级。
- 静态文件服务存在 TOCTOU 窗口：路径校验与实际打开文件分离，docroot 可写场景下校验后文件可被替换；挂载点前缀不剥离，非根挂载路由把完整请求路径拼进 docroot，路径语义错误。
- 应用开发者收不到 multipart/form-data 请求的结构化内容：表单上传（含文件字段）的 body 只是一坨字符串，无 parts 访问器，业务侧被迫手撕 boundary。

## Solution

H5 治理轮第一子批（H5.1），补齐服务端静态能力，均为现有能力的补全：

- StaticFileServlet 输出强 ETag（mtime+size）与 Last-Modified；评估 If-None-Match（RFC 优先级先于 If-Modified-Since），命中返 304；既有 IMS/304、Range、Cache-Control 行为保持。
- 文件打开改为「先 open 后 fstat」：文件身份来自已打开的 fd，校验/ETag/大小全用 fstat，闭合 TOCTOU 窗口；body-file 写出路径增 fd 入口承接所有权。
- 挂载点前缀经尾段捕获剥离：尾段非空用尾段作相对路径，根挂载/精确路由维持全路径。
- 新增 multipart/form-data 解析：独立纯函数解析器 + 请求装配区 parts 访问器，整体受既有 body 上限封顶；只做解析与访问，不做流式落盘。

## User Stories

1. 作为静态文件消费者，我希望 GET 响应携带 ETag 头，从而能用 If-None-Match 做精确缓存验证。
2. 作为静态文件消费者，我希望 GET 响应携带 Last-Modified 头，从而能用 HTTP 日期做缓存验证与展示。
3. 作为静态文件消费者，我希望同一文件未变化时多次请求得到同一 ETag，从而缓存验证稳定可信。
4. 作为静态文件消费者，我希望文件内容/大小/修改时间变化后 ETag 随之变化，从而不会命中过期缓存。
5. 作为静态文件消费者，我希望发送匹配的 If-None-Match 时收到 304 且无 body，从而节省带宽。
6. 作为静态文件消费者，我希望 If-None-Match 与 If-Modified-Since 同时存在时 ETag 验证优先（RFC 9110），从而验证语义符合规范。
7. 作为静态文件消费者，我希望 If-None-Match 不匹配时收到正常 200 全量，从而拿到最新内容。
8. 作为静态文件消费者，我希望 If-None-Match 的弱比较（W/ 前缀）与 `*` 通配按 RFC 生效，从而与各类客户端兼容。
9. 作为静态文件消费者，我希望 304 响应仍携带 ETag/Last-Modified/Cache-Control，从而能续接缓存元数据。
10. 作为静态文件消费者，我希望 HEAD 请求同样参与条件验证（304 无 body），从而探测缓存不拉内容。
11. 作为静态文件消费者，我希望非 GET/HEAD 方法不做 304 验证（按 RFC 前置条件语义之外维持现状），从而写方法语义不被缓存逻辑污染。
12. 作为静态文件消费者，我希望既有 If-Modified-Since/304、单区间 Range 206/416、多区间回落 200、Cache-Control 行为不变，从而既有下载与缓存行为零回归。
13. 作为静态文件消费者，我希望条件验证命中 304 优先于 Range 处理，从而不会收到矛盾的 206+304 语义。
14. 作为安全敏感的运营者，我希望文件校验与打开基于同一已打开 fd（先 open 后 fstat），从而 docroot 可写场景下校验后替换文件的 TOCTOU 窗口被闭合。
15. 作为安全敏感的运营者，我希望符号链接仍被拒绝（O_NOFOLLOW），从而 docroot 外文件不可经 symlink 读出。
16. 作为安全敏感的运营者，我希望 docroot 前缀校验保留，从而路径穿越防护不退化。
17. 作为应用开发者，我希望把 StaticFileServlet 挂在尾段通配路由（如 `/assets/*`）时请求 `/assets/a.png` 服务 docroot 下的 `a.png`，从而挂载点前缀被正确剥离。
18. 作为应用开发者，我希望根挂载与精确路由挂载行为不变（全路径语义），从而既有用法零回归。
19. 作为应用开发者，我希望 multipart/form-data 请求体被解析为 parts，从而 handler 能按字段名取内容。
20. 作为应用开发者，我希望每个 part 暴露 name、filename（如有）与自身头域，从而能区分普通字段与文件字段。
21. 作为应用开发者，我希望 part body 按原始字节保真（含二进制），从而文件上传内容不被文本化破坏。
22. 作为应用开发者，我希望 boundary 从 Content-Type 参数正确解析（含引号包裹形态），从而与标准客户端兼容。
23. 作为应用开发者，我希望畸形 multipart（缺 boundary、截断、非法分隔）返回 400，从而错误请求被明确拒绝而非静默吞掉。
24. 作为应用开发者，我希望 multipart 整体受既有 body 上限（8MB）封顶，从而超大上传不会耗尽内存。
25. 作为应用开发者，我希望 application/x-www-form-urlencoded 的既有 form 装配不受影响，从而两类表单互不干扰。
26. 作为应用开发者，我希望非 multipart 请求的 parts 访问器返回空，从而能安全区分请求类型。
27. 作为框架维护者，我希望本子批不引入回归、既有 62 个测试目标保持全绿，从而能力补全不破坏现有行为。
28. 作为框架维护者，我希望每项能力都有先红后绿的端到端用例锁定，从而缺口被真实覆盖而非仅改实现。
29. 作为框架维护者，我希望 Q38 登记闭环（If-Match/If-Unmodified-Since/If-Range 残留注记），从而缺陷登记簿与实际能力一致。

## Implementation Decisions

- **子批定位**：H5 治理轮拆 H5.1/H5.2/H5.3 三子批（2026-11-25 裁决）；本轮仅 H5.1，顺序 ETag（Q38）先行 → multipart 次之。H5.2（连接池+重定向、WS 客户端）、H5.3（异步 Servlet、限流+XFF）各自独立子轮。服务端 SNI/ALPN 已移出 H5 挂 HTTP/2 远期轮。
- **ETag 生成**：强 ETag，fstat 的 mtime+size 十六进制拼装（nginx 同款算法形态）；不做内容哈希（大文件成本与 sendfile 预读冲突）。ETag 与 Last-Modified 同源于 fstat，保证一致性。
- **条件请求**：输出 ETag + Last-Modified；评估 If-None-Match，RFC 9110 优先级——If-None-Match 存在时先于 If-Modified-Since 判定；弱比较（W/ 前缀等价）与 `*` 通配按 RFC；304 限 GET/HEAD，携带 ETag/Last-Modified/Cache-Control，无 body；304 判定先于 Range 处理。既有 IMS/304 逻辑保留为 ETag 缺失时的回退腿。**If-Match/If-Unmodified-Since/If-Range 不在本轮**，登记残留挂后续。
- **TOCTOU 闭合**：StaticFileServlet 先以 O_NOFOLLOW|O_RDONLY 打开解析后路径，再 fstat 校验 regular file/大小/修改时间——文件身份来自已打开 fd，校验与写出同源；realpath docroot 前缀校验保留。body-file 写出路径增 fd 入口（所有权移交，sendfile 路径天然用 fd）；原路径入口保留兼容。symlink 由 O_NOFOLLOW 拒绝（ELOOP → 403/404 按既有错误语义）。
- **挂载前缀剥离**：servlet 取请求尾段（H4.2 落地的尾段捕获访问器），非空时以尾段作 docroot 相对路径；尾段空（根挂载/精确路由）维持全路径。不引入 Servlet 规范 servletPath/pathInfo 全套语义。
- **multipart 形态**：独立纯函数解析器（新模块，与 HttpUtil 同级）；HttpRequest 增装配区 finishMultipart/getParts（仿 finishForm 先例，解析器完成点调用）；part 暴露 name/filename/自身头域/body 字节。boundary 从 Content-Type 参数解析（含引号形态）。整体受 maxBodyBytes（8MB）封顶；畸形 → 400。只解析+访问，不做流式落盘/大文件上传。
- **登记与收尾**：Q38 闭环（残留 If-* 注记）；H4 spec §7 已更新三子批结构；不自动 commit/push，完成后展示变更摘要由维护者确认。

## Testing Decisions

- **唯一 seam（最高层、复用既有，grilling 已确认）**：真实 socket 客户端 ↔ 运行中 HttpServer 的端到端请求/响应。只测外部可观测契约——请求进、状态/头/体出：
  - ETag/Last-Modified：GET 断言头存在与格式；重复请求 ETag 稳定；文件改动后 ETag 变化。
  - 条件验证：INM 匹配 → 304 无 body 含缓存头；INM 不匹配 → 200；INM+IMS 共存 → ETag 优先；W/ 弱比较与 `*`；HEAD 同判；304 先于 Range。
  - TOCTOU/symlink：symlink 拒；docroot 前缀穿越拒（既有回归）。
  - 前缀剥离：`/assets/*` 挂载请求 `/assets/a.png` 命中 docroot `a.png`；根挂载回归。
  - multipart：handler 回显 parts（name/filename/body）；boundary 引号形态；畸形 400；urlencoded 不受影响；非 multipart parts 空。
- **好测试的标准**：断言请求/响应外部契约（状态码、头、体、parts 内容），不断言内部成员或调用次序；红面先证明缺口存在，再转绿。
- **prior art**：StaticFileServletTest（TempWebRoot 夹具）、HttpStreamingTest（Range/sendfile 端到端）、HttpServerTest（路由/端到端）、HttpMessageTest/HttpRequestParserTest（装配先例，仅参考形态，测试仍走端到端 seam）。
- **被 exercised 的模块**：StaticFileServlet（ETag/条件/TOCTOU/前缀）、body-file 写出路径（fd 入口）、HttpRequest 装配区 + 新 MultipartParser（multipart）、HttpRequestParser（完成点装配）。
- **回归门槛**：每项红→绿；常规全量 62 目标 + TSAN/ASAN 全量；变体切换强制重链。

## Out of Scope

- If-Match / If-Unmodified-Since / If-Range（登记残留挂 H5 后续）。
- 流式上传/大文件落盘（multipart 仅内存解析，8MB 封顶）；嵌套 multipart（multipart/mixed）。
- H5.2/H5.3 全部功能；服务端 SNI/ALPN（已移 HTTP/2 远期轮）。
- Servlet 规范 servletPath/pathInfo 全套语义（仅尾段剥离最小方案）。
- 弱 ETag（W/）生成（本轮只生成强 ETag；弱比较仅用于 INM 评估兼容）。
- 挂账观察项：Q42/Q43/Q45/Q44。

## Further Notes

- ETag 与 TOCTOU 修复共用 fstat 是设计耦合点：文件身份、校验、缓存验证元数据三者同源，避免「校验的 file ≠ 写出的 file ≠ ETag 描述的 file」。
- fd 入口与 sendfile 路径自然衔接：sendfile 本就用 fd，所有权移交后无需二次 open。
- 尾段剥离的正确性受益于 H4.2 的 %2F 默认拒绝：解码子路径无歧义分隔符。
- multipart 装配仿 finishForm 先例（解析器完成点调用、EqualCI 匹配 content-type），保持装配模式一致。
- 缺陷登记簿 docs/known-issues.md 与本 tracker 分工不同：Q38 闭环属实施期登记动作，不在本 spec 发布范围内。
- 本 spec 依据 grilling 两轮裁决（范围 Q1–Q4 + 设计 Q1–Q6）综合产出；seam 选用单一既有端到端 HTTP seam。

## Comments

- 2026-11-25 实现完成（/implement + tdd，单一端到端 seam，逐片 red→green）：
  - ETag（Q38）：ETag+Last-Modified 输出（FormatHttpDate 新工具）；INM 评估（RFC 优先级/W 弱比较/*）；304 限 GET/HEAD 先于 Range；TOCTOU open+fstat + setBodyFile/Range fd 入口（所有权移交）；挂载前缀经 getPathTail 剥离。
  - multipart：MultipartParser 新模块（ExtractBoundary/ParseMultipart）+ finishMultipart/getParts 装配 + parser 完成点钩子（畸形→400）；Swap 补 m_parts 移交（TakeRequest 路径）。
  - 验证：常规 / TSAN / ASAN 全量各 62/62；HttpStreamingTest 20、StaticFileServletTest 6、HttpRequestParserTest 24、HttpMessageTest 23 无回归。
  - 登记：known-issues Q38 闭环（If-Match/If-Unmodified-Since/If-Range 残留注记）。
- 2026-11-25 双轴审查修复：Spec 轴——304 Cache-Control 锁定用例（maxAgeSec>0）、ExtractQuotedParam 参数边界修复（filename= 内部子串误匹配 name=，红→绿）、缺头体空行畸形收紧（合法空 body part 保留，红→绿）；Standards 轴——FdGuard noncopyable+noexcept、fd 入口 RAII 化+fstat 单点化、HttpDateNow 委托 FormatHttpDate、ParseMultipart/finishMultipart 补 ATTR_NODISCARD、StaticFileServlet.h 类注释同步。修复后三矩阵复验各 62/62。