# H4.2 · HTTP 功能欠账（多区间 Range / 尾段捕获 / Session 注入 / %2F）

Status: resolved

## Problem Statement

框架使用者在 HTTP 模块上遇到四个"已实现但有缺口"的能力短板：

- 静态文件消费者发起多区间 `Range` 请求时收到 416，拿不到任何可用内容。
- 应用开发者用尾段通配路由（`*`）时，Servlet 只能命中路由却取不到匹配的子路径，头文件宣称的"多段捕获"与实现不符。
- 应用开发者挂了 SessionFilter 后，首个无 Cookie 请求会被 Filter 与 handler 各建一个会话，导致会话状态丢失、Set-Cookie 与 handler 实际使用的会话不一致。
- 安全敏感的运营者发现请求 path 中的编码斜杠 `%2F` 解码后与真实分隔符不可区分，可能绕过前缀/模式路由与路径规范化。

## Solution

以一个独立完整性轮（H4.2）补齐这四项，均为现有能力的补全而非新功能：

- 多区间 `Range` 回落为 200 全量响应（RFC 允许忽略 Range），单区间 206 与非法 416 保持不变。
- 尾段通配路由把匹配到的剩余子路径暴露给 Servlet，经一个专用请求访问器读取。
- 在请求上建立会话注入通道：SessionFilter 解析出的会话写入请求，handler 的 getSession 优先取注入会话，从而首请求只产生一个会话。
- 默认拒绝 path 中的编码斜杠（400），并提供配置开关允许显式放行；仅作用 path，query 不受影响。

## User Stories

1. 作为静态文件消费者，我希望多区间 Range 请求返回 200 全量文件而非 416，从而在客户端请求多个区间时仍能拿到可用内容。
2. 作为静态文件消费者，我希望单区间 Range 请求继续返回 206 与正确的 Content-Range，从而既有的分段下载行为不受影响。
3. 作为静态文件消费者，我希望真正不可满足/非法的单区间仍返回 416，从而能识别错误的区间请求。
4. 作为静态文件消费者，我希望多区间回落时不携带 Content-Range 头，从而响应语义与"忽略 Range 的全量响应"一致。
5. 作为应用开发者，我希望尾段通配路由（`*`）把匹配到的剩余子路径暴露出来，从而我的 Servlet 能据此服务或检查捕获的路径（如静态资产路由）。
6. 作为应用开发者，我希望捕获的尾段是解码后、按 `/` 拼接、不含前导 `/` 的子路径，从而能直接当作相对路径使用。
7. 作为应用开发者，我希望命名路径参数（`{name}`）与尾段捕获并存且行为不变，从而混合模式路由照常工作。
8. 作为应用开发者，我希望没有尾段通配的路由返回空尾段，从而能区分通配与非通配路由。
9. 作为应用开发者，我希望 SessionFilter 在首个无 Cookie 请求上创建的会话与 handler 看到的是同一个，从而不会每首请求误建两个会话。
10. 作为应用开发者，我希望 handler 里 getSession 直接返回注入的会话而不再新建，从而我在 Filter 中设置的会话状态对 handler 可见。
11. 作为应用开发者，我希望未挂 SessionFilter 时 getSession 仍按"未命中即新建"工作，从而既有的独立用法不变。
12. 作为应用开发者，我希望 Set-Cookie 中的会话 id 与 handler 实际使用的会话一致，从而后续请求能续接同一会话。
13. 作为安全敏感的运营者，我希望请求 path 中的编码斜杠默认被拒绝并返回 400，从而编码分隔符无法绕过前缀/模式路由或路径规范化。
14. 作为有合法需求的运营者，我希望通过配置开关显式放行编码斜杠，从而能按需回到此前的宽松行为。
15. 作为 API 消费者，我希望 query 参数中的 `%2F` 不受影响，从而正常的查询编码继续可用。
16. 作为运营者，我希望编码斜杠拒绝对所有 handler（含静态文件）统一生效，从而不存在静默接受编码分隔符的路径。
17. 作为框架维护者，我希望这四项补全不引入回归、既有 62 个测试目标保持全绿，从而完整性修复不破坏现有能力。
18. 作为框架维护者，我希望每项修复都有先红后绿的端到端用例锁定，从而缺口被真实覆盖而非仅改实现。

## Implementation Decisions

- **轮次定位**：独立完整性轮 H4.2，仅含上述四项；推进顺序按风险低→高：多区间回落 → 尾段捕获 → Session 注入 → `%2F`。不并入其他 H5 功能。
- **多区间 Range**：检测到 Range 含逗号（多区间）即忽略 Range，按全量文件返回 200，不携带 Content-Range。单区间 206、非法/超界 416 的既有判定保持不变。不实现 `multipart/byteranges` 多区间响应体。
- **尾段捕获**：模式匹配器在命中尾段 `*` 时，捕获固定前缀之后的剩余段，按 `/` 拼接为解码子路径（无前导 `/`）。经一个**专用请求访问器**（`getPathTail`）暴露，底层用与既有路径参数/远端地址一致的 mutable 装配槽承载；分发器在路由参数装配阶段写入。路由匹配与优先级规则不变，仅新增捕获产出。
- **Session 注入**：请求上新增 mutable 会话槽（set/get）。SessionFilter 将其解析（找到或新建）的会话写入请求；SessionManager 的"解析或新建"入口**先查注入槽**，命中即返，未命中再走 Cookie 查找。无 Filter 时的独立 create-on-miss 语义不变。选用最小专用槽而非通用属性/上下文 map。
- **`%2F` 拒绝**：在**解析期、URL 解码之前**检测原始 path 是否含 `%2F/%2f`（大小写不敏感），命中且开关关闭时返回 400（与既有 `%00` 拒绝同处）。新增服务端配置布尔 `allowEncodedSlash`，默认 `false`（拒绝），并传递到解析器。仅作用 path，query 不受影响；对所有 handler 统一生效。
- **登记与收尾**：实施时同步缺陷登记簿（关闭 Q41、Q35、Q38 的多区间部分、Session 双会话部分），并从 H5 清单移除已完成的重叠项（多区间、Session 注入）；D/Q 编号续接。不自动 commit/push，完成后展示变更摘要由维护者确认。

## Testing Decisions

- **唯一 seam（最高层、复用既有）**：真实 socket 客户端 ↔ 运行中 HttpServer 的**端到端请求/响应**。只测外部行为——请求进、状态/头/体出——不测实现细节。四项均在此 seam 观测：
  - 多区间：发 `Range: bytes=0-1,5-6` → 期望 200 全量、无 Content-Range。
  - 尾段：注册 `/prefix/*` 路由、handler 回显 `getPathTail()` → 期望响应体为捕获子路径。
  - Session：挂 SessionFilter + 回显会话 id 的 handler，发首个无 Cookie 请求 → 期望 Set-Cookie 与 handler 所见会话一致、仅一个会话。
  - `%2F`：path 含 `%2F` → 默认期望 400；`allowEncodedSlash=true` 时期望放行。
- **好测试的标准**：断言请求/响应的外部可观测契约（状态码、头、体、会话一致性），不断言内部成员或调用次序；红面先证明缺口存在，再转绿。
- **prior art**：HttpStreamingTest（Range/sendfile 端到端）、HttpServerTest（路由/端到端）、SessionTest（Cookie/会话流）、HttpsServerTest（TLS 变体）。
- **被 exercised 的模块**：StaticFileServlet（多区间）、ServletDispatcher + HttpRequest（尾段）、Session/SessionFilter/SessionManager + HttpRequest（会话）、HttpRequestParser + HttpServerConfig（`%2F`）。
- **回归门槛**：每项红→绿；常规全量 62 目标回归 + sanitizer 抽验；`%2F` 为行为变更，单独红面并验证开关两侧。

## Out of Scope

- `multipart/byteranges` 多区间响应体（仅回落 200 全量）。
- 其他 H5 功能：异步 Servlet、HttpClient 连接池+重定向、multipart 解析、限流+XFF、WS 客户端、ETag 完整（Q38 余项：ETag/If-None-Match/Last-Modified/TOCTOU/挂载点前缀剥离）、服务端 SNI/ALPN。
- 通用请求属性/上下文 map（本轮选最小会话槽）。
- 命名通配语法（如 `{name*}`）；仅支持末段 `*`。
- 观察/挂账项：Q42（TSAN 间歇）、Q43（Stop 队列序竞态）、Q45（TLS WANT_READ）、Q44（证书到期监控）。

## Further Notes

- `%2F` 拒绝是默认安全的行为变更，故置于本轮最后并单独红面；`allowEncodedSlash` 开关保留显式放行路径。
- 尾段捕获的正确性受益于 `%2F` 拒绝：解码子路径中不再混入歧义分隔符，拼接无二义。
- Session 注入沿用既有 mutable 装配模式（const 请求引用贯穿 Filter 链，装配写入对下游可见），与路径参数/远端地址槽同构。
- 缺陷登记簿 `docs/known-issues.md` 与本 tracker 分工不同：关闭 Q 编号属实施期登记动作，不在本 spec 发布范围内。
- 本 spec 依据 grilling 已敲定的共同理解（Q1–Q6）综合产出；seam 选用单一既有端到端 HTTP seam。

## Comments

- 2026-11-23 实现完成（/implement + tdd，单一端到端 HTTP seam，逐片 red→green）：
  - ① 多区间 Range→200 全量（RangeMultiIntervalFallsBack200）；单区间 206/416 不变。
  - ② 尾段捕获 getPathTail（TailWildcardCapture / TailWildcardWithNamedParam）。
  - ③ Session 注入槽（SessionFilterHandlerShareSession；SessionTest 回归通过）。
  - ④ %2F 默认拒绝 + allowEncodedSlash 开关（EncodedSlashRejectedByDefault / AllowedByConfig / InQueryUnaffected）。
  - 验证：常规 / TSAN / ASAN 全量各 62/62 通过；HttpRequestParserTest、ServletDispatcherTest、SessionTest 无回归。
  - 登记：known-issues 关闭 Q41/Q35；H4 spec §7 H5 清单移除已完成重叠项（多区间、Session 注入）。
- 2026-11-24 收账：实现已随 `2ce0341`（dev）落库，Status 翻为 resolved；审查遗留命名裁决（http 模块局部 snake_case 例外）登记于 `.claude/rules/code_style.md` §2。