# HTTP 模块三期（H3）设计与计划：协议深度轮（五单元 · 交互式）

> 状态：已批准（2026-10-28），实施中
> 关联：[一期设计](2026-10-27-http-design.md) · [二期设计](2026-10-28-http-r2-design.md)（均闭环，基线 HEAD 4c402bb）

## 1. 需求背景

H2 收官登记的 H3 清单落地：**协议深度补全**——WebSocket 帧层（H2 主线承诺，U7 钩子空转
两轮后兑现）、通用流式（响应写路径终态：sendfile/chunked 拉模型）、gzip 压缩、TCP backlog
清偿（Q34）与观察项批量关闭（Q36/Q40/Q41 等）。

**架构审视结论**（本方案设计过程中定稿）：
- TLS 独立 **H4**（TCPConnection 注入 SslSession+内存 BIO 形态——TCP 层透明包装因
  `TCPConnection::ptr` 类型死穴被否决、独立 Session 层因发向不可桥被否决）
- 异步 Servlet/连接池/multipart/限流/XFF → **H5 治理轮**；流式 BodyProvider 契约按 H4
  异步可组合形态预留

**目录裁决**（用户原则）：`src/net/websocket/`（bus::ws，独立 RFC 协议、单向只读依赖 http）；
gzip/流式留 http（报文语义本体）；TLS H4 时建 src/net/ssl/。

## 2. 已裁决设计决策（用户确认 2026-10-28）

| # | 决策点 | 裁决 |
|---|---|---|
| 1 | H3 切分 | **方案一「协议深度轮」**：U1 TCP backlog → U2 gzip → U3 通用流式 → U4 WS 帧层 → U5 收尾批 |
| 2 | zlib 依赖 | **确认引入**（系统 -lz；H2 遗留裁决落地，不新增 3rd/ 入树） |
| 3 | TLS 时机 | **H4 独立轮** |
| 4 | 砍除项 | D 异步 Servlet/G 连接池/H multipart/I 限流/J XFF → H5 治理轮；Q42 维持观察 |

我职责内定（设计代理建议已审阅采纳）：WS 握手 SHA1/Base64 **自实现**（RFC 双向量锁定，
本方案无 OpenSSL）；流式 **writeComplete 驱动拉模型**（单连接在途一个 chunk，内存有界天然
背压）；gzip **默认关**（零破坏构造性保证）；WS 生命周期归 WS 层（U7 契约）。

## 3. 复用的既有资产与纪律

- UpgradeHandler 钩子（U7：会话先摘表/leftover 精确透出/101 回调方拼发）——U4 桥接地基
- HttpServer 中心扫描模式（base loop TimerManager+快照）——WsServer sweep 镜像复用
- DumpHead/Dump 字节序全等用例（U5 产出）——U3 重构的零破坏护栏
- 纯函数层先行测试模式（HttpUtil/WsFrame 同型）；三 sanitizer 矩阵；D52/Q43 起登记

## 4. 处理单元设计（U1–U5）

（接口与机制细节与已批准实施计划一致：U1 setContext/inputBuffer/addCloseObserver+两绕行
删除；U2 HttpCompression 纯函数+sendResponse 判定链+请求解压 zlib bomb 防护；U3
TCPConnection::sendFile+输出状态机三态+chunked 拉模型+Range 单区间+Q36/Q40/Q41 同点清偿；
U4 src/net/websocket/ 四文件对+桥接序列+RFC 全表校验；U5 收尾批。签名级定义见实施计划。）

每单元六步协议；U3 第 1 步须呈全量 diff；U1 后 TCP 12 目标先行回归。

## 5. 交互式处理协议

同一期/二期：每单元一轮六步（问题卡/方案确认 → 代码确认 before/after → TDD 红 → 实现 →
三矩阵验证 → 登记）。

## 6. 验证策略

| 时机 | 动作 |
|---|---|
| 每单元 | `ctest -R "Http\|Ws\|TCP"` + 全量回归 |
| U1 后 | TCP 12 目标先行 + 三矩阵 |
| U3/U4 后 | TSAN/ASAN 全矩阵 |
| 新目标 | HttpCompressionTest/HttpStreamingTest/WsFrameTest/WebSocketServerTest（52→56） |
| 终验 | N×{常规,TSAN,ASAN} + repeat（新目标） |

## 7. 风险与对策

R1 U3 零破坏（gzip 默认关+sendfile 字节流等价+Dump 全等护身+全量 diff）；R2 WS 桥接竞态
（重绑先于 leftover 同线程+无会话早退安全垫+定向用例）；R3 SHA1/codec 正确性（RFC 双向量）；
R4 zlib bomb/分片炸弹（解压上限=maxBodyBytes+maxMessageBytes 封顶 1009）；R5 U1 触 TCP 基座
（全 additive+先行回归）；R6 节奏超支（UTF-8 校验默认关/U5 可顺延）。

## 8. 进度记录

| 单元 | 状态 | 备注 |
|---|---|---|
| U1 TCP backlog | **完成**（2026-10-28） | 常规 52/52（TCPConnectionTest 2 新用例：ContextSetGet/CloseObserverFires；既有单飞回灌用例行为等价验证）；Q34 闭环：setContext/getContext+inputBuffer()+addCloseObserver（close_cb 主槽不动，多播观察链 handleClose 尾触发+context 统一 reset）；HttpServer 热路径 context 化（map 留 sweep 快照）+pendingInput 拷贝消除（字节留连接缓冲天然背压，积压上限检查等价迁移）；实施修正：多轮 python 替换锚点失配改逐块落盘+行号替换 |
| U2 gzip | **完成**（2026-10-28） | 常规 53/53（HttpCompressionTest 5 用例 + HttpServerTest 4 端到端）；zlib 接线 find_package(ZLIB)；判定链 shouldGzip 纯函数化；请求解压上限=maxBodyBytes（bomb 用例 413 锁定）；实施修正：GzipConfig 声明序前移（先于 HttpServerConfig）、HttpRequest::m_body 置 mutable（dispatch 前置解压装配）、测试 readAll→readResponse（keep-alive 按 CL）与 maxBody/炸弹尺寸自纠 |
| U3 通用流式 | **完成**（2026-10-30） | 常规 54/54 + TSAN/ASAN 3 目标绿（HttpStreamingTest 6 用例：8MB sendfile 校验和/chunked 100 段解码/1.0 close-delimited/provider abort/Range 三形态+416）；**实施修正六项（均为实测定位）**：①sendfile EBADF——resp 栈析构关 fd，resp 移入 session 持有②chunk 帧十进制→RFC 9112 十六进制③帧格式判定——TE 是 DumpHead 动态产物不落 HeaderMap，会话显式记 providerChunked④write_cb RunInLoop 同线程立即执行致 pumpProvider 同步重入 94 层栈溢出→改 QueueInLoop⑤1.0 close_after 首段后半关截断流→closeAfter 延迟到流尽⑥shutdown 补 pending_file 检查；Q36/Q40/Q41 同点清偿（bytes 精确记账/截断记 shutdownNs/高水位已挂）；竞态 U3-R（探针尾包×Stop 队列序）以 settle 消解并登记 |
| U4 WebSocket 帧层 | **完成**（2026-10-30） | 常规 56/56 + ASAN 10/10 + TSAN 56/56；新目录 src/net/websocket/ 四对（Frame/Handshake/Session/Server）+ 两测试目标（WsFrameTest 7 用例/WebSocketServerTest 7 用例）；**实施修正三项**：①SEGFAULT 根因=context 类型混淆（WS 接管 setContext 覆盖 HttpSession→HttpServer write_cb 类型混淆调垃圾指针）——WS 回退自有注册表（name→entry mutex）不碰 context ②handleUpgrade 曾覆盖业务 close 回调→透传 ③~WsServer 兜底闭包 std::function 可拷贝（shared_ptr keeper）；SHA1/Base64 自实现（RFC 双向量锁）；**TSAN 终验修复三项**：①Stop 闭包捕 this×对象析构竞态（UAF）→自包含闭包（shared keeper）②close observer 捕 this×~WsServer 析构 m_sessions 竞态→weak_ptr 判活③HttpClientTest 用例 Stop 后 set observer 竞态→顺序调整 || U5 收尾批 | **完成**（2026-10-30） | Q39 闭环（Cookie 全属性段注入检查+用例）；Q37 闭环（MSG_PEEK \| MSG_DONTWAIT 精确断言）；Q42 维持观察（6 轮复测零复现） |

**H3 收官（2026-10-30）**：U1–U5 全部落地。测试目标 52→56（HttpCompressionTest/HttpStreamingTest/WsFrameTest/WebSocketServerTest）；新目录 src/net/websocket/（bus::ws，四对文件）；zlib 系统依赖落地（find_package(ZLIB)）。H4=TLS 独立轮（形态已定稿）；H5=治理轮。

## H4/H5 预告

H4=TLS 独立轮（src/net/ssl/，形态已定稿；异步 Servlet 弹性位）；H5=治理轮（连接池+重定向/
multipart/限流+XFF/WS 客户端+wss/ETag 完整）。
