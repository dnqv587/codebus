# HTTP 模块四期（H4）设计与计划：TLS 独立轮（九单元 · 交互式）

> 状态：已批准（2026-11-05），实施中
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
| U0 | P0 小账收尾批：删 build/（权限拦截转用户终端执行）；CLAUDE.md 待实现段同步；docs/README.md 补 6 份 spec 索引+编号指针 D52/Q44；Q36 以代码为真相源核实闭环；根 README 死链/模块表/36→56/View 表述/TimerManager 示例参数；StaticFileServlet.h 陈旧注释；3 份 spec 状态头翻转收官；本设计文档骨架 | docs/*、README.md、CLAUDE.md、StaticFileServlet.h | 无（文档）；StaticFile 抽查回归 | 实施中 |
| U1 | OpenSSL 依赖接入（find_package + OpenSSL::SSL/Crypto）；SslContext（进程级初始化守卫、createServer/createClient 工厂、TLS1.2 下限、禁重协商、ACCEPT_MOVING_WRITE_BUFFER\|ENABLE_PARTIAL_WRITE、关压缩；证书失败抛 Exception 含 ERR 队列 dump） | 根/src CMakeLists.txt、src/net/ssl/SslContext.{h,cc} | SslContextTest（新增） | — |
| U2 | SslSession 核心：SSL* + rbio/wbio 内存对；startHandshake/feedCiphertext/pullOutgoing/advance（Want 五态映射）/decrypt/encrypt（明文所有权移交内部队列）/flushPendingPlaintext/sendCloseNotify；SSL_get_error 错误分类私有层 | src/net/ssl/SslSession.{h,cc} | SslSessionTest（新增：双 session 内存 BIO 交叉对拍——无 socket 无 loop） | — |
| U3 | TCPConnection 注入【最高风险：触 TCP 基座】：attachSsl（仅 Connecting 接受）+ isSslEstablished/sslHandshaking；handleRead 双路径（密文→feed→advance→decrypt→明文 Buffer→msg_cb，SkipRead 语义复用）；sendInLoop 双路径（encrypt 密文走既有直写/排队/高水位同构）；handleWrite 接续 flush；shutdown=close_notify 后半关；handleClose 头部插 close_notify（M1/D38 零改动）；sendFile TLS 降级链 pumpPendingFileTls（pread 64KB→encrypt，WANT_WRITE 保写事件）；TcpServer.cc:105/TcpClient.cc:322 装饰回调 | TCPConnection.{h,cc}、TcpServer.{h,cc}、TcpClient.{h,cc} | TlsConnectionTest（新增）+ TCPConnectionTest 增 2 用例；**全量 diff 呈现 + TCP 12 目标先行回归** | — |
| U4 | HttpsServer：TlsConfig 聚合 + decorator attach；sweep 握手死线分支（问题卡二选一：独立分支【推荐】vs 复用 requestStartNs）；sendfile 降级链端到端验证 | HttpServer.{h,cc} | HttpsServerTest（新增：mini TLS 客户端过渡） | — |
| U5 | HttpClient https：TlsConfig{enabled, verifyPeer=true, caFile}；客户端 SNI（SSL_set_tlsext_host_name）；TLS Fatal → failInflight + m_client->stop() 停重试（防「TCP 通→握手败→retry」死循环，问题卡确认默认值） | HttpClient.{h,cc} | HttpsClientTest（新增：对 U4 回环，verifyPeer=true 走真实路径） | — |
| U6 | WS sweep 优化（独立分支可穿插）：SessionEntry 增 weak_ptr\<TCPConnection\>（弱引用无环）；sweep 兑现三死线——idlePingSec 主动 ping（WsSession::ping 已有）/ pongTimeout 硬关 / closeReply 硬关（均替换「仅 WARN」）；WsSession 增 setPongCallback 清 pingSentNs | WsServer.{h,cc}、WsSession.{h,cc} | WebSocketServerTest 增 4 用例（无新目标） | — |
| U7 | wss 端到端（**零代码预期**——D-1+D-3 红利验收）：HttpsServer(tls) + setUpgradeHandler(WsServer::handleUpgrade) 组装；桥接缝隙（101 经 TLS 排队时序等）在此修 | 预期无（缝隙修补除外） | WssServerTest（新增） | — |
| U8 | 收尾批：known-issues 登记（新 D/Q 自 **D53/Q45** 起——D52/Q44 已用）+ 头部编号指针同步；本文档补 §8 进度记录+收官头+H5 预告；CLAUDE.md（HTTPS/TLS→已实现、56→62、目录补 src/net/ssl/）；docs/README.md 补状态；根 README 特性/依赖补 TLS；终验全量三矩阵 + 新目标 --repeat until-fail:10 | docs/*、CLAUDE.md、README.md | 全量 | — |

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
| U2–U8 | — | — | — |
