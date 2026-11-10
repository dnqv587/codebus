# HTTP 模块四期（H4）设计与计划：TLS 独立轮（九单元 · 交互式）

> 状态：已收官（U0–U8 全部完成，2026-11-10；62 测试目标三矩阵全绿）
> 关联：[一期设计](2026-10-27-http-design.md) · [二期设计](2026-10-28-http-r2-design.md) · [三期设计](2026-10-28-http-r3-design.md)（均收官）
> 用户裁决（2026-11-05）：本轮 = P0 小账收尾 + H4 TLS 开工 ｜ TLS 库 = **OpenSSL**（系统库 find_package，与 zlib 同款路径） ｜ 异步 Servlet 归 H5 ｜ 授权删除 build/（权限系统拦截，由用户终端执行）

## 1. 需求背景

H3 收官（56 测试目标三矩阵全绿）后，HTTP 模块剩余工作盘点为三块：**①P0 小账收尾批**（文档/注释/索引低风险清理）、**②H4 TLS 独立轮**（本设计主体）、**③H5 治理轮**（下轮预告，见 §7）。

TLS 注入形态于 r3 定稿：`src/net/ssl/`，**SslSession + 内存 BIO 注入 TCPConnection**。两个已否决方案不再复议：TCP 层透明包装（TCPConnection::ptr 类型死穴）、独立 Session 层（发向不可桥）。

## 2. 根基决策

| # | 决策 | 内容 | 理由 |
|---|---|---|---|
| D-1 | 握手状态机内嵌 SslSession | `Connected` 仍 = TCP 建立完成；TLS 三态（kHandshaking/kEstablished/kClosed）在 SslSession 内部；握手期上层明文由 SslSession 排队、完成自动 flush；握手死线由服务器层 sweep 经 conn 查询兜底 | TCPConnection 状态机零触碰（M1/D38 时序纪律不动）；HttpServer 会话管理/sweep/升级桥接零改动，wss 只是组装 |
| D-2 | 注入方式 = `attachSsl` setter + 装饰回调 | TcpServer/TcpClient 增协议无关 `setNewConnectionDecorator(function<void(const TCPConnection::ptr&)>)`；attachSsl 在 established 后断言拒绝 | 既有装配点与全部测试零改动；与 UpgradeHandler「挂回调不穿透类型」先例同型；装饰回调在 base loop 线程执行，SslSession 构造后随 RunInLoop(connectEstablished) 交接 ioLoop，交接窗口无触碰（登记纪律） |
| D-3 | HttpsServer = `HttpServerConfig::tls` 配置聚合 | `TlsConfig{enabled, certFile, keyFile, handshakeTimeoutSec}`；非独立类 | 避免 ~15 个接口（Get/Post/…/setDispatcher/addFilter/setUpgradeHandler/Session）透传包装的双类永久同步；ssl 头仅 .cc include（前置声明 + unique_ptr 成员），明文用户运行时零开销 |

## 3. 单元分解（每单元六步协议：问题卡→方案确认→代码确认 before/after→TDD→三矩阵验证→登记）

| 单元 | 范围 | 主要文件 | 测试目标 | 状态 |
|---|---|---|---|---|
| U0 | P0 小账收尾批：删 build/（权限拦截转用户终端执行）；CLAUDE.md 待实现段同步；docs/README.md 补 6 份 spec 索引+编号指针 D52/Q44；Q36 以代码为真相源核实闭环；根 README 死链/模块表/36→56/View 表述/TimerManager 示例参数；StaticFileServlet.h 陈旧注释；3 份 spec 状态头翻转收官；本设计文档骨架 | docs/*、README.md、CLAUDE.md、StaticFileServlet.h | 无（文档）；StaticFile 抽查回归 | 已完成 |
| U1 | OpenSSL 依赖接入（find_package + OpenSSL::SSL/Crypto）；SslContext（进程级初始化守卫、createServer/createClient 工厂、TLS1.2 下限、禁重协商、ACCEPT_MOVING_WRITE_BUFFER\|ENABLE_PARTIAL_WRITE、关压缩；证书失败抛 Exception 含 ERR 队列 dump） | 根/src CMakeLists.txt、src/net/ssl/SslContext.{h,cc} | SslContextTest（新增） | 已完成 |
| U2 | SslSession 核心：SSL* + rbio/wbio 内存对；feedCiphertext/pullOutgoing/advance（Want 五态映射）/decrypt/encrypt（明文所有权移交内部队列）/flushPendingPlaintext/sendCloseNotify；SSL_get_error 错误分类私有层 | src/net/ssl/SslSession.{h,cc} | SslSessionTest（新增：双 session 内存 BIO 交叉对拍——无 socket 无 loop） | 已完成 |
| U3 | TCPConnection 注入【最高风险：触 TCP 基座】：attachSsl（仅 Connecting 接受）+ isSslEstablished/sslHandshaking；handleRead 双路径（密文→feed→advance→decrypt→明文 Buffer→msg_cb，SkipRead 语义复用）；sendInLoop 双路径（encrypt 密文走既有直写/排队/高水位同构）；handleWrite 接续 flush；shutdown=close_notify 后半关；handleClose 头部插 close_notify（M1/D38 零改动）；sendFile TLS 降级链 pumpPendingFileTls（pread 64KB→encrypt，WANT_WRITE 保写事件）；TcpServer.cc:105/TcpClient.cc:322 装饰回调 | TCPConnection.{h,cc}、TcpServer.{h,cc}、TcpClient.{h,cc} | TlsConnectionTest（新增）+ TCPConnectionTest 增 2 用例；**全量 diff 呈现 + TCP 12 目标先行回归** | 已完成 |
| U4 | HttpsServer：TlsConfig 聚合 + decorator attach；sweep 握手死线分支（问题卡二选一：独立分支【推荐】vs 复用 requestStartNs）；sendfile 降级链端到端验证 | HttpServer.{h,cc} | HttpsServerTest（新增：mini TLS 客户端过渡） | 已完成 |
| U5 | HttpClient https：TlsConfig{enabled, verifyPeer=true, caFile}；客户端 SNI（SSL_set_tlsext_host_name）；TLS Fatal → failInflight + m_client->stop() 停重试（防「TCP 通→握手败→retry」死循环，问题卡确认默认值） | HttpClient.{h,cc} | HttpsClientTest（新增：对 U4 回环，verifyPeer=true 走真实路径） | 已完成 |
| U6 | WS sweep 优化（独立分支可穿插）：SessionEntry 增 weak_ptr\<TCPConnection\>（弱引用无环）；sweep 兑现三死线——idlePingSec 主动 ping（WsSession::ping 已有）/ pongTimeout 硬关 / closeReply 硬关（均替换「仅 WARN」）；WsSession 增 setPongCallback 清 pingSentNs | WsServer.{h,cc}、WsSession.{h,cc} | WebSocketServerTest 增 3 用例（无新目标） | 已完成 |
| U7 | wss 端到端（**零代码预期**——D-1+D-3 红利验收）：HttpsServer(tls) + setUpgradeHandler(WsServer::handleUpgrade) 组装；桥接缝隙（101 经 TLS 排队时序等）在此修 | 预期无（缝隙修补除外） | WssServerTest（新增） | 已完成 |
| U8 | 收尾批：known-issues 登记（新 D/Q 自 **D56/Q45** 起——D52–D55/Q44 已用）+ 头部编号指针同步；本文档补 §8 进度记录+收官头+H5 预告；CLAUDE.md（HTTPS/TLS→已实现、56→62、目录补 src/net/ssl/）；docs/README.md 补状态；根 README 特性/依赖补 TLS；终验全量三矩阵 + 新目标 --repeat until-fail:10 | docs/*、CLAUDE.md、README.md | 全量 | 已完成 |

## 4. 测试策略

### 证书：仓库检入 test/certs/

自签 **CA**（ca.crt/ca.key）+ **server 证书**（SAN=DNS:localhost,IP:127.0.0.1——必含 IP 条目，R12）+ **不受信证书**（验证失败用例）+ README（用途/再生成命令/到期日）+ 生成脚本 scripts/gen-test-certs.sh 检入备轮换。`-days 7300`（2046-11 到期），known-issues 登记到期监控。

取舍：检入 vs CMake 期 openssl CLI 生成——检入可复现、CI 无 openssl CLI 依赖（curl/libwebsockets 同法），选检入。

### 目标矩阵：56 → 62（+6）

| 目标 | 单元 | 形态 |
|---|---|---|
| SslContextTest | U1 | 纯配置/加载断言 |
| SslSessionTest | U2 | 内存 BIO 双 session 交叉对拍（无 socket 无 loop） |
| TlsConnectionTest | U3 | socketpair + 装饰 attach，loop 驱动全双工 |
| HttpsServerTest | U4 | 本地回环（mini TLS 客户端） |
| HttpsClientTest | U5 | HttpsServer↔HttpClient 回环 |
| WssServerTest | U7 | TLS 之上 WS 回环 |

### 三矩阵注意点

- **TSAN**：SSL/SSL_CTX/BIO 严格单线程归属（SslSession 全生命周期 ioLoop 线程）；TlsConnectionTest/HttpsServerTest 为重点目标；die_after_fork=0 + setarch -R 照旧
- **ASAN**：SSL_CTX/SSL/BIO/ERR 队列显式配对释放优先；OpenSSL 进程级 reachable 噪声时用既有 NO_LEAK_CHECK 目标级开关
- 变体切换强制重链纪律照旧（rm bin/*Test lib/libcodebus.so）

## 5. 风险清单

| # | 风险 | 对策 |
|---|---|---|
| R1 | SSL_write 部分写重试语义（WANT_WRITE 后调用方缓冲已失效） | 明文所有权移交 SslSession 内部队列（encrypt 返回接管字节数）+ SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER；SslSessionTest 部分写模拟用例锁定 |
| R2 | 双 BIO 缓冲增长/背压 | 读侧密文仍走 m_input_buffer（H1 readv 减速语义不变）；写侧密文进 m_output_buffer 高水位 WARN+关原样生效；spec 记录两缓冲生命周期 |
| R3 | TLS1.2 重协商中断非阻塞 IO | SSL_OP_NO_RENEGOTIATION 默认禁（U1 基线）；1.3 天然无 |
| R4 | SNI/ALPN 范围 | 客户端 SNI 纳入 U5；服务端 SNI 回调与 ALPN 出轮登记 H5+（单证书即可；ALPN 仅 HTTP/2 需要） |
| R5 | 慢速握手攻击（TCP 建立后 trickle ClientHello） | U4 sweep 握手死线 + 坏证书 Fatal 即关 |
| R6 | TLS Fatal × TcpClient retry 死循环 | U5：Fatal → m_client->stop() 停重试 |
| R7 | 测试证书有效期 | -days 7300 + 检入再生成脚本 + known-issues 到期监控登记 |
| R8 | ASAN 泄漏噪声（OpenSSL 全局分配） | 显式配对释放优先；噪声时目标级 NO_LEAK_CHECK |
| R9 | U3 触 TCP 基座 | TLS 全 additive（无 attach 行为字节等价）+ 全量 diff 呈现 + TCP 12 目标先行回归 + 56 零破坏验收 |
| R10 | close_notify × D38 时序 | handleClose 头部插入式 close_notify、对端 RST/EOF 不死等；终态先写纪律不动 |
| R11 | sendfile 降级链 WANT_READ（TLS1.3 罕见） | 保写事件等待 + 读事件驱动兜底；登记 Q 观察项 |
| R12 | 测试证书 SAN 缺 IP 条目致 verifyPeer 失败 | 生成命令强制 -addext subjectAltName=DNS:localhost,IP:127.0.0.1（脚本固化） |

## 6. 顺序与依赖

```
U0 → U1 → U2 → U3【全量 diff + TCP 12 目标先行回归】→ U4 → U5 → U7 → U8（串行主线）
                    U6（独立分支，穿插于 U2/U3 交互确认间隙）
```

每单元验证命令模板：`cmake --build build -j$(nproc) && ctest --test-dir build -R "Ssl|Tls|Https|Wss|TCP|Http|Ws" --output-on-failure`，随后全量。

## 7. 本轮不做（H5 预告 + 挂账）

- **H5 治理轮**：异步 Servlet（用户裁决归此）、连接池+重定向、multipart、限流+XFF、WS 客户端（明文）、ETag 完整（Q38：ETag/If-None-Match/Last-Modified 输出/TOCTOU/挂载点前缀剥离）、多区间 Range→200 全量回落、Session 双会话 req 上下文注入通道、服务端 SNI/ALPN
- **挂账观察**：Q42（TSAN 间歇，多轮零复现维持观察）、Q43（U3-R Stop 队列序竞态 settle 消解未根治）、Q33（HttpDateNow locale）、Q35（UrlDecode 日志洪水）、D8/D36（登记不修）、UDP W3（非 HTTP）

## 8. 进度记录

| 单元 | 状态 | 完成日期 | 备注 |
|---|---|---|---|
| U0 | 已完成 | 2026-11-05 | 文档批 8 项全落地（CLAUDE.md 待实现段/docs 索引+指针/Q36 核实闭环/根 README 9 死链+模块表+56 目标+View 表述+TimerManager 示例/StaticFileServlet.h 注释/3 spec 状态头/本文档骨架）；build/ 由用户自行恢复标准路径；**插曲 D52**：抽查暴露 d1cdcb7 的 Config 硬依赖击穿 17 目标——用户裁决「get 未加载不抛返 NullValue」修复，锁定用例翻转，常规 56/56 全绿（TSAN/ASAN 由 U1 起的矩阵覆盖） |
| U1 | 已完成 | 2026-11-05 | OpenSSL 3.0.13 接入（find_package + OpenSSL::SSL/Crypto，ldd 确认）；SslContext 工厂落地（TLS1.2 下限/禁重协商/部分写模式基线；证书失败抛 SslError 含 ERR 队列）；test/certs/ 六件套检入（SAN 含 IP，R12 消解）+ gen-test-certs.sh；SslContextTest 5 用例；常规 57/57 全绿 + ASAN SslContextTest 零泄漏；Q44 到期监控登记 |
| U2 | 已完成 | 2026-11-09 | SslSession 落地（SSL\*+内存 BIO 对、三态状态机、advance 五态 Want 映射、明文队列=握手期排队、close_notify 优雅关闭）；接口精化两处：去 startHandshake（ctor 设 connect/accept 状态，client 首 flight 由首次 advance 产出）、明文队列职责收敛为握手期排队+WANT_WRITE 防御兜底。SslSessionTest 8 用例（双 session 内存对拍：握手/首 flight/坏证书/1MB 双向/握手前排队/半速传输/close_notify/fatal 吸收）；修复 sendCloseNotify 误挡对端先关后的己方回执，并修正测试传输 helper 的舍入滞留与握手期 alert 驱动。常规 58/58 全绿 + ASAN SslSession/SslContext 零泄漏 |
| U3 | 已完成 | 2026-11-10 | TCPConnection TLS 注入落地（attachSsl/双路径 handleRead・sendInLoop・handleWrite/sendFile TLS 降级链 pumpPendingFileTls/close_notify 半关与 handleClose 补发/flushSslOutgoing 含高水位）+ TcpServer/TcpClient setNewConnectionDecorator（协议无关装配挂点）。**对抗审查 4 视角 6 修正折入**（A1 attach Release 守卫/A2 best-effort 吞错误/A3 文件链背压界/A4 kWrite 再驱/A5 查询非内联防 openssl 头泄漏/A6 排空先 flush）；另增 SslSession::hasOutgoing/plainBuffer。**实施迭代修复两处产品缺陷**：①handleClose close_notify 守卫 isEstablished 误挡「对端先关后的回执」（双向优雅关闭断裂——ShutdownSendsCloseNotify 抓获）；②**D53：Socket::write/writev 未防 SIGPIPE**（对端已关连接写=杀进程，明文路径潜在同暴露——PeerAbruptTeardownSafe 实测击中，修为 send/sendmsg MSG_NOSIGNAL）。TlsConnectionTest 7 用例 + TCPConnectionTest 增 2（attach 时序死亡测试/inputBuffer 重定向）。**三矩阵里程碑**：常规 59/59 + TSAN 59/59（首轮 1 次间歇未复现，fork 死亡测试嫌疑，Q42 同族观察）+ ASAN 59/59 零泄漏 + Tls/TCPConnection repeat×10 稳定 |
| U4 | 已完成 | 2026-11-10 | HttpsServer 落地：TlsConfig 聚合（决策 D-3）+ ctor 装配期建上下文（证书错误 Start 前暴露）+ decorator attach；**握手死线方案 A 落定**（B 不可行：TLS 未完成握手无明文通道可回 408，且无法区分握手中/握手后空闲会误杀 keep-alive）；sweep 首分支硬关（handshakeTimeoutSec 默认 10s）。**纪律修正**：SslSession::m_state 原子化（base loop sweep 跨线程查询 vs ioLoop 写——Q30 同族），契约放宽为「状态查询跨线程安全，其余方法单线程」；TCPConnection 增 sslHandshakeElapsedNs。HttpsServerTest 7 用例（GET/POST 往返/keep-alive 复用/8MB sendfile 降级链字节全等/握手死线硬关/坏证书拒/gzip over TLS 解压校验）+ mini TLS 客户端过渡形态。**实测教训**：①TSAN 抓获测试自身数据竞争（m_responses loop 线程产/测试线程读——互斥+拷贝取值修复）；②一次变体混链事故（TSAN 跑后未强制重链即改码——CLAUDE.md 已载纪律的现场重演，重链后消）。验证：常规 60/60 + TSAN 60/60 + ASAN 抽验 6/6 零泄漏 |
| U5 | 已完成 | 2026-11-10 | HttpClient https 落地：ClientTlsConfig{enabled, verifyPeer=true, caFile} + ctor 装配期建上下文/decorator attach；SNI + 证书主机名校验（SslSession::setPeerName——IP 走 set1_ip_asc 命中 SAN IP、DNS 走 set1_host）；**R6 TLS fatal 停重试**（m_client->stop() + m_tls_dead 拒新请求；独立标志不复用 m_stopped——避免 Stop() 清理路径被跳过的 D40 违约）；HttpClient 增 connection() 观测访问器。HttpsClientTest 6 用例（verify 真实路径含 IP 主机名校验/POST/keep-alive 同指针复用/不受信证书拒且 2.5s 观察窗无重连死循环/Reset 重连新会话/verify=false 容忍）。**实测击出存量缺陷 D54**（明文对照确认非 TLS 特有）：断连→重连窗口暂存请求永久滞留 + 断连误判死暂存区——Request 重构为 park+TryFlushHandoff 双触发、failInflight 增 fail_parked 语义分层；HttpClientTest 13/13 守卫重构。验证：常规 61/61 + TSAN 抽验 14/14 零 race + ASAN 抽验 5/5 零泄漏 |
| U6 | 已完成 | 2026-11-10 | WS sweep 三死线兑现（明文零 TLS 依赖，独立分支）：SessionEntry 增 weak_ptr\<TCPConnection\>（弱引用防环——sweep 主动 ping/硬关执行通道，连接死 lock 失败跳过）；WsSession 增 setPongCallback（任意 pong 清死线）；sweep 三分支替换「仅 WARN」——close-reply 超时 conn->close() 硬关 / pong 超时发 close(1001) 后硬关 / idle 真发 session->ping（原「业务侧自驱心跳」简化退役）。WebSocketServerTest 增 3 用例（IdlePing 到达+pong 清死线存活 4.5s 验证 / PongTimeout 3s 内 EOF / CloseReplyTimeout 硬关）+ useConfig 短死线夹具。**ASAN 击出并修复一处环泄漏**：pong 回调若强捕 entry 则 entry↔session 成环（表摘除后泄漏）——弱捕修复；TSAN Ws 系零 race。验证：常规 61/61 + ASAN/TSAN Ws 系全绿 |
| U7 | 已完成 | 2026-11-10 | wss 端到端验收：组装零产品代码预期达成——HttpsServer(tls) + setUpgradeHandler(WsServer::handleUpgrade) 即得 wss。WssServerTest 5 用例（101 握手 Accept 校验/8KB echo/关闭握手/**U6 心跳叠加 TLS**（ping 到达+pong 清死线存活）/256KB 分片重组）；客户端为自研「TLS+WS」测试形态（TcpClient+decorator+自写服务端帧头解析——ParseFrameHeader 内建服务端规则拒未掩码帧不适用于客户端侧）。**实测击出 D55（U3 引入）**：TLS 收包「数据+close_notify 同段」尾段被吞（先关后交付）——wss close 握手零字节回达击中，交付序翻转修复。测试迭代：UTF-8 载荷合法性（validateText 正确拒任意字节流 1007——测试改合法载荷）。验证：Wss 5/5 + 常规全量 62/62 + TSAN/ASAN 抽验零 race 零泄漏 |
| U8 | 已完成 | 2026-11-10 | 文档与索引同步：CLAUDE.md 测试目标 56→62、补齐 TLS/WSS 清单与 src/net/ssl/ 目录、待实现段移除 HTTPS/TLS；根 README/docs 索引更新 H4 已落地；§3 单元状态统一，§8 补回 U2 记录。终验证据：常规全量 62/62（78.58s）；TLS/WSS 7 目标（SslContext/SslSession/TlsConnection/HttpsServer/HttpsClient/WssServer/WebSocketServer）`--repeat until-fail:10` 全绿（191.36s）；TSAN 全量 62/62（84.77s，ThreadSanitizer WARNING=0）；ASAN 全量 62/62（71.45s，AddressSanitizer/LeakSanitizer ERROR=0）；强制重链回常规终验 62/62（79.93s）。无新增 D/Q，下一可用编号保持 D56/Q45 |
