# HTTP H4.1 正确性修复设计与计划

> 状态：已收官（U0–U6 全部完成，2026-11-11；62 测试目标三矩阵通过）
> 关联：[H4 TLS 设计](2026-11-05-http-r4-tls-design.md)（功能轮已收官）
> 范围：TLS 密文保序/高水位重入/TLS 文件链/HTTP 流式单飞/HttpClient 超时/WS 线程亲和/握手死线周期

## 1. 背景与目标

H4 的 TLS、HTTPS、WSS 功能已完成，62 目标三矩阵通过。查缺发现现有测试未覆盖的交织时序，本轮回不扩展 H5 功能，只对正确性缺陷做最小修复。每单元先建立可控红面，再修根因并登记证据。

## 2. 根因与修复边界

| # | 根因 | 修复边界 |
|---|---|---|
| 1 | `flushSslOutgoing()` 在旧输出非空时直写新密文，破坏 TLS record 顺序 | 旧输出非空只按序追加，由 handleWrite 串行排空 |
| 2 | 高水位通知在缓冲搬动中同步执行，close 重入后继续使用旧 Slice | 高水位通知统一 QueueInLoop，先完成缓冲和事件记账 |
| 3 | handleWrite 空输出+文件在途未按 TLS 分流，可能调用明文 sendfile | TLS 必须进入 pumpPendingFileTls |
| 4 | 文件未完成先 close_notify，且 encrypt 失败仍推进文件记账 | 文件完成后才 close_notify；encrypt 失败立即错误关闭，不记账 |
| 5 | 文件/provider 未全程保持 responsePending，pipeline 响应可插入流中 | IO/worker 都按流式在途单飞，流尽后解除并回灌 |
| 6 | 响应超时走优雅 disconnect，TLS 握手排队使连接卡 Disconnecting | 超时 failInflight 后硬 close，保留 Connector retry |
| 7 | WsServer sweep 在 base loop 直接操作非线程安全 WsSession | 死线计算留 base loop，ping/close 投递 conn 所属 ioLoop |
| 8 | HTTPS sweep 周期未纳入 handshakeTimeoutSec，实际死线被拉长 | TLS 开启时将握手死线计入扫描周期最小值 |

## 3. 单元与测试

| 单元 | 内容 | 主要红面 |
|---|---|---|
| U1 | TLS 输出保序 + 高水位重入 | A 尾段积压后发送 B/close_notify，验证 A→B→close 顺序；小高水位 close 无 UAF/事件复活 |
| U2 | TLS 文件降级链 | 空输出+文件在途不发明文；HTTP/1.0 与 Connection: close 的 8MB 慢读文件完整交付 |
| U3 | 文件/provider 流式单飞 | IO 与 worker 下同包 pipeline 第二请求不得插入第一响应；流尽正常回灌 |
| U4 | HttpClient 超时确定性断连 | 对端不回 ServerHello，超时回调后必须 Disconnected，后续请求不永久 park |
| U5 | WS sweep 回 ioLoop | thread_num=2 下 pong 超时与 close/违规交织，TSAN 无 race/UAF/重复 close |
| U6 | 握手死线周期 + 小账与终验 | 不缩小 keepAlive/header 超时也能按 handshakeTimeoutSec 关闭 |

## 4. 固定纪律

- M1/D38/D27/Q30/D9 不变；明文路径由既有 62 目标回归。
- 不在缓冲搬动途中执行用户回调；WsSession 只允许 ioLoop 访问。
- 闭包弱引用自包含，不延长 WsServer/连接/SessionEntry 的不当生命周期。
- 变体切换先 `find bin -maxdepth 1 -type f -delete && rm -f lib/libcodebus.so`。
- 每单元：红面→最小修复→相关目标→常规全量→登记；最终三矩阵与相关目标 repeat×10。

## 5. 进度记录

| 单元 | 状态 | 红面与验证记录 |
|---|---|---|
| U0 | 已完成 | H4.1 spec 建立；基线常规 62/62（79.58s），工作区初始干净 |
| U1 | 已完成 | D56 红面：旧输出 1,074,176B 时新 record 4,118B 直写成功且交付超时；D57 TLS/明文红面均为 send_completed=false。修复：旧输出非空只追加保序，高水位统一 QueueInLoop。验证：TLS/TCP 相关 2/2、常规全量 62/62（79.92s）、ASAN 相关 2/2 |
| U2 | 已完成 | D58：握手期 sendFile 回退明文分支后红面握手损坏，分流修复后通过；D59：Connection: close 8MB 红面超时，文件完成后才 close_notify+encrypt 失败不记账后通过。验证：TLS/HTTPS 2/2、常规全量 62/62（79.84s）、ASAN 相关 2/2 |
| U3 | 已完成 | D60：IO/worker 文件/provider 四红面均成立（体污染/段缺失）；进一步实测 worker provider 段序 B-A-C，确认首段 worker/后续 ioLoop 的线程乱序。修复为文件/provider 全程单飞、流尽回灌，且响应发送与流式推进统一回 ioLoop。验证：4/4 + HttpServer/Worker/Streaming 3/3 + 常规 62/62（78.97s）+ TSAN 相关 2/2 |
| U4 | 已完成 | D61：裸 TCP 不回 ServerHello，红面持续 Disconnecting + 第二请求 park；超时改硬 close 后 Disconnected/retry 观察成立、第二请求超时收口，既有明文/HttpsClient 回归通过。常规全量 62/62（79.48s） |
| U5 | 已完成 | D62：64 连接多 loop 对齐第三次 sweep，TSAN 实证 Consume 与 base close 对 m_close_sent 数据竞争；另修正测试 TearDown 以 RunInLoopSync 替代假排空（曾报 removeConnectionInLoop UAF）。ping/close 改 weak 闭包投递 ioLoop 后零警告；WS/WSS TSAN 2/2、常规全量 62/62（84.79s）。SslSession 跨线程契约注释已收窄 |
| U6 | 已完成 | D63：默认其他配置下红面 5s 未关，sweep shortest 纳入 handshakeTimeoutSec 后 1.01s 关闭。终验：常规 62/62（83.21s）；8 个相关目标 repeat×10 全绿（296.02s）；首轮 TSAN 暴露 D64（observer 跨线程写 std::function），RunInLoop 装配修复后全量 62/62、WARNING=0（80.90s），另有 Q42 迟到 newConnection UAF 间歇记录保留观察；ASAN 62/62、ERROR=0（65.31s）；回常规终验 62/62（74.20s）。小账：Q33/Q35/Q38/Q41 状态纠正、Q45 WANT_READ 观察登记、README/索引同步。**独立审查修复**：D65 文件截断/sendfile 失败不关连接，明文/TLS 红面后统一 handleError，常规+TSAN+ASAN 再全绿；保序测试增加未排空断言并修正回调状态析构顺序。下一编号 D66/Q46 |
