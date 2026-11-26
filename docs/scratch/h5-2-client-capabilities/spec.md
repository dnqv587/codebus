# H5.2 · 客户端三件（HttpClient 连接池+重定向 / WS 客户端明文）

Status: ready-for-agent

## Problem Statement

框架使用者在 HTTP/WS 客户端侧遇到三组能力缺口：

- API 消费者用 HttpClient 只能单连接单在途：并发请求被 CAS 拒绝或 park 排队，无法利用多连接并行；连接管理（建连/重试/超时）虽已有，但并发调度缺位。
- API 消费者收到 3xx 只能自己解析 Location 再发请求：重定向跟随完全手工，且无跳数保护。
- 应用开发者没有 WebSocket 客户端：服务端 WsServer 已就绪，但框架内无法发起 ws 连接，测试与业务只能裸 socket 手撕帧。

## Solution

H5 治理轮第二子批（H5.2），补齐客户端三件，顺序池+重定向先行、WS 客户端次之：

- 单 host 连接池：管理 N 个 HttpClient 实例，空闲优先+FIFO 排队调度并发请求，不改单实例状态机。
- 重定向跟随：HttpClient 层 opt-in 配置，同 host 自动跟随（RFC 方法语义），跨 host 与超跳数可观测交付。
- WS 客户端：TcpClient 底座，握手/帧/会话复用既有服务端组件的客户端变体，明文 ws。

## User Stories

1. 作为 API 消费者，我希望经连接池并发发起多个请求（至 N 连接并行），从而吞吐不受单在途限制。
2. 作为 API 消费者，我希望池优先分发到空闲连接，从而请求延迟最小。
3. 作为 API 消费者，我希望全忙时请求按 FIFO 排队、响应完成即分发，从而突发流量不丢请求。
4. 作为 API 消费者，我希望连接惰性创建至上限 N，从而空闲时不浪费 fd。
5. 作为 API 消费者，我希望传输失败以 nullopt 原样交付（池层不自动重试），从而失败语义与单实例一致、由我决定重试。
6. 作为 API 消费者，我希望池 Stop 时在途回调 exactly-once、排队请求 nullopt 交付，从而停机无悬挂回调。
7. 作为 API 消费者，我希望经配置设定 maxConnections，从而按服务端承载力调并发。
8. 作为 API 消费者，我希望单实例 HttpClient 行为完全不变（含默认不跟随重定向），从而既有用法零回归。
9. 作为 API 消费者，我希望同一连接 keep-alive 续用多请求，从而池不每请求建连。
10. 作为 API 消费者，我希望默认不跟随重定向（3xx 原样交付），从而既有 RedirectNotFollowed 语义兼容。
11. 作为 API 消费者，我希望经 followRedirects 开关 opt-in 自动跟随，从而按需启用。
12. 作为 API 消费者，我希望 303 重定向转为 GET 并丢 body，从而符合 RFC 9110。
13. 作为 API 消费者，我希望 301/302 的 POST 重定向转为 GET（历史兼容），从而与浏览器行为一致。
14. 作为 API 消费者，我希望 307/308 重定向保持方法与 body，从而语义严格遵循 RFC。
15. 作为 API 消费者，我希望 GET/HEAD 重定向保持方法，从而探测请求不变形。
16. 作为 API 消费者，我希望丢 body 时同步移除 Content-Length/Content-Type，从而重定向请求帧合法。
17. 作为 API 消费者，我希望同 host 重定向链透明跟随、只交付最终响应，从而业务代码无感。
18. 作为 API 消费者，我希望跨 host 3xx 原样交付（不跟随），从而单 host 池边界清晰、跨域决策留给我。
19. 作为 API 消费者，我希望超 maxRedirects 时交付最后一个 3xx 响应，从而失败可观测而非静默 nullopt。
20. 作为 API 消费者，我希望 Location 缺失/非法时交付原 3xx，从而畸形重定向不崩溃。
21. 作为 API 消费者，我希望经 maxRedirects 配置跳数上限（默认 5），从而防重定向循环耗尽。
22. 作为 API 消费者，我希望重定向链保留原请求头（除方法/body 变更连带），从而认证/自定义头续接。
23. 作为应用开发者，我希望 WsClient.connect(path) 完成 ws 握手后触发 onOpen，从而确认连接就绪。
24. 作为应用开发者，我希望握手 Accept 校验失败/非 101 时经回调报错，从而不进入假就绪状态。
25. 作为应用开发者，我希望 sendText 发出的帧被服务端正确接收（客户端掩码），从而文本消息互通。
26. 作为应用开发者，我希望 sendBinary 二进制保真，从而帧内容不被文本化破坏。
27. 作为应用开发者，我希望 onMessage 收到服务端文本/二进制消息（服务端帧不掩码），从而双向通信。
28. 作为应用开发者，我希望 ping 后 pong 可观测（回调），从而链路探活。
29. 作为应用开发者，我希望 close(code, reason) 发起关闭握手并经 onClose 收口，从而优雅关闭。
30. 作为应用开发者，我希望服务端主动 close 时 onClose 携带 code/reason，从而感知对端关闭。
31. 作为应用开发者，我希望收到掩码的服务端帧（协议违规）触发 1002 关闭，从而客户端视角校验严格。
32. 作为应用开发者，我希望分片消息通过 onMessage 前完成重组，从而业务只见完整消息。
33. 作为应用开发者，我希望 WsClient 生命周期契约与 TcpClient 一致（回调内只许 stop、析构安全），从而无 UAF/死锁。
34. 作为应用开发者，我希望 v1 无自动心跳（按需 ping），从而行为可预测、 scope 聚焦。
35. 作为框架维护者，我希望服务端 WsFrame 校验规则零放宽（1002 全表锁定不动），从而客户端变体不引入服务端回归。
36. 作为框架维护者，我希望本子批不引入回归、既有 62 目标全绿，从而能力补全不破坏现有行为。
37. 作为框架维护者，我希望每项能力先红后绿端到端锁定，从而缺口被真实覆盖。

## Implementation Decisions

- **子批定位**：H5.2 顺序池+重定向 → WS 客户端；H5.3（异步 Servlet/限流XFF）独立子轮；wss 留 TLS 客户端轮。
- **池形态（ADR-0003）**：单 host 连接池管理 N 个 HttpClient 实例（不改单实例状态机——单在途 CAS 与池并发是不同层）；入口镜像 HttpClient（Get/Post/Request + optional<HttpResponse> 回调）；空闲实例优先，全忙 FIFO 排队、响应完成分发；实例惰性创建至 maxConnections；传输失败 nullopt 原样交付，池层无自动重试（Connector retry 管连接层重连）；Stop 在途 exactly-once、排队 nullopt；配置新增 HttpPoolConfig 平铺聚合体。
- **重定向（ADR-0003）**：HttpClient 层跟随、仅同 host（m_host const；跨 host 3xx 原样交付登记残留）；拦截点在响应完成回调触发前（CAS 已释放，重入 Request 合法）；HttpClientConfig 扩展 followRedirects=false / maxRedirects=5；方法语义：303→GET 丢 body、301/302 POST→GET 丢 body（历史兼容）、307/308 保方法保 body、GET/HEAD 不变；丢 body 同步移除 CL/CT；超跳数/Location 缺失非法 → 交付最后/原 3xx（可观测）；重定向链保留原头（除方法/body 连带）。
- **WS 客户端**：WsClient 底座 TcpClient（HttpClient 对 Upgrade 直接拒，改造面大）；生命周期契约照搬 TcpClient（make_shared、回调内只许 stop、析构须 Disconnected 后于 loop 外）；connect(path) 异步握手：随机 16 字节 base64 key + GET Upgrade 请求构造 + 101 校验（ComputeAcceptKey 复用，HttpResponseParser kUpgrade 解析）；回调 onOpen/onMessage(WsMessage)/onClose(code,reason)；发送面镜像 WsSession（sendText/sendBinary/ping/close）。
- **帧/会话角色变体**：WsFrame 增客户端解析变体（掩码位须 0 否则 1002）+ 掩码发送变体（随机 key + ApplyMask 复用）；WsSession 增角色标志切换 Consume 掩码规则；服务端现有校验零放宽（WsFrameTest 1002 全表不动）。
- **心跳**：v1 无自动 ping/sweep（调用方按需 ping，pong 回调可观测）；自动心跳登记残留。
- **登记与收尾**：RedirectNotFollowed 现状用例保留（默认关兼容）；残留注记（跨 host 重定向、自动心跳）；ADR-0003 记录分层决策；不自动 commit/push，完成后展示变更摘要由维护者确认。

## Testing Decisions

- **唯一 seam（最高层、复用既有，grilling 已确认）**：真实客户端 ↔ 运行中服务端的端到端。只测外部可观测契约——响应/回调/帧内容，不测内部调度细节：
  - 池：并发请求经多连接并行到达服务端（服务端观测连接数/请求序）；全忙排队后分发；Stop 收口 exactly-once。
  - 重定向：服务端发 3xx+Location，客户端交付最终响应（同 host）/原 3xx（跨 host/超跳数/非法 Location）；方法/body 变更经服务端回显观测。
  - WS：WsClient ↔ 既有 WsServer 桥接服务端；握手/文本/二进制/ping-pong/close 双向/协议违规 1002/分片重组。
- **好测试的标准**：断言回调交付物与服务端观测（状态/头/体/帧/关闭码），不断言内部成员或调度次序；红面先证明缺口存在再转绿。
- **prior art**：HttpClientTest（双 LoopThread + 真实 HttpServer 对端 + CallbackLatch + DisconnectedObserver FIFO 屏障）、WebSocketServerTest（HttpServer+WsServer 桥接 SetUp，裸 socket 帧客户端可镜像）、WsFrameTest（帧向量）、HttpsClientTest（TLS 边界参考，本轮明文不涉及）。
- **被 exercised 的模块**：新 HttpConnectionPool、HttpClient（重定向拦截）、新 WsClient、WsFrame/WsSession（角色变体）、WsHandshake（复用）。
- **回归门槛**：每项红→绿；常规全量 62 目标 + TSAN/ASAN 全量；变体切换强制重链；服务端 WsFrame/WsServer 用例零回归。

## Out of Scope

- wss（TLS WS 客户端，留 TLS 客户端轮）；跨 host 重定向跟随（残留）；池层自动重试；多 host 池映射/驱逐；自动心跳/sweep（残留）；H5.3 全部；服务端 SNI/ALPN（已移 HTTP/2）。
- 挂账观察项：Q42/Q43/Q45/Q44。

## Further Notes

- 池与单实例是不同层：池管实例生命周期与调度，单实例 CAS/handoff/超时语义原样保留——D54 handoff 契约与 exactly-once 纪律贯穿池 Stop。
- 重定向拦截在 CAS 释放后重入 Request 合法（侦察确认），无需改单在途状态机。
- WsSession 无连接引用（send_fn 注入）天然可镜像客户端；角色变体而非放宽校验是 WsFrameTest 1002 全表锁定的必然要求。
- 客户端掩码发送复用 ApplyMask（双向幂等）；随机 key 经系统随机源。
- 缺陷登记簿与 tracker 分工不同：残留注记属实施期登记动作。
- 本 spec 依据 grilling 两轮裁决（范围 Q1–Q5 + 设计 Q1–Q7）综合产出；ADR-0003 记录池/重定向分层。

## Comments

- 2026-11-25 实现完成（/implement + tdd，客户端端到端 seam，逐片 red→green）：
  - 重定向：HttpClientConfig followRedirects/maxRedirects；同 host 拦截重入（doRequest 跳数不清零）；RFC 方法语义（303/301/302 POST→GET 丢 body 清 CL/CT，307/308 保留）；超跳数/缺 Location/跨 host 交付 3xx；HeaderMap::removeHeader + HttpRequest::clearBody/setUrl 支撑。
  - 连接池：HttpConnectionPool 新模块（N 实例调度，空闲优先+FIFO+惰性创建+Stop done cb 收口）；重复 Stop cb 链入兑现（body+TearDown 双 Stop 形态）。
  - WS 客户端：WsClient 新模块（TcpClient 底座，握手装配 loop 化，发送面 QueueInLoop 串行）；WsFrame 客户端变体（ParseFrameHeaderClient/AppendFrameMasked，视角参数化公共实现）；WsSession 角色标志；WsHandshake GenerateSecKey/BuildUpgradeRequest/Validate101Response。
  - 关键修复：WsSession 协议违规分支丢弃 m_input（残留字节污染后续解析——客户端等 echo 场景实测击中）；测试夹具 weak_ptr 持服务端连接（强引用致 fd 不关 FIN 不发）。
  - 验证：常规 / TSAN / ASAN 全量各 64/64（新增 HttpConnectionPoolTest/WsClientTest 两目标，CLAUDE.md 计数同步）；WsFrameTest/WebSocketServerTest 服务端零回归。
- 2026-11-25 双轴审查修复：Spec 轴——HEAD+303 豁免改 GET（红→绿）、跳数重置移至 CAS 成功后（被拒并发不削弱保护）、相对 Location 目录解析（红→绿）、池 Stop FIFO 屏障（done 先于实例清理则析构违约，结构修复+parked 锁定用例）、覆盖补全（301/308/头保留/非 101/keep-alive 复用）、RandomMaskKey 改系统随机源逐帧抽取；Standards 轴——WsClient 成员/setter Doxygen+析构 noexcept+connect QueueInLoop 化、AppendFrameImpl 长度编码单点参数化、空分支清理、pgrade 注释、hook 词边界锚定+jq 缺失告警。修复后三矩阵复验各 64/64。