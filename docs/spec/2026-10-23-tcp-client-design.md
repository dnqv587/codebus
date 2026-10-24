# TCP Client 模块设计与计划（全栈一轮 · 交互式）

> 状态：**已完成**（U0-U4 全部闭环，2026-10-23）
> 关联：[2026-10-22-tcp-server-design.md](2026-10-22-tcp-server-design.md)（服务端栈与审查修复轮 P9 已全部闭环）

## 1. 需求背景

服务端栈完整交付后，`TcpClient` 是待实现清单剩余项。本轮交付**对称的异步 TCP 客户端**：Connector 非阻塞连接器（含 retry 退避）+ TcpClient 生命周期编排 + 统一回调接口，与 muduo TcpClient/Connector 形态对齐。

**用户要求**：沿用六步交互协议（单次一个处理单元；方案与代码确认后才动工）。

## 2. 现状基础（已核查）

- **TCPConnection 状态机已为客户端铺路**（M1：初始态 Connecting、connectEstablished 期望 Connecting、析构允许未建立态销毁）——但缺**客户端失败收尾路径** `connectDestroyed`
- Socket::NIO_TCP 支持非阻塞 connect（EINPROGRESS）；getLocalAddr/getRemoteAddr/getsockopt(SO_ERROR) 可用
- TimerManager（addTimer(cb, steady_clock 时点, interval)）可驱动退避重试；构造/析构须在 loop 线程（D6/D18 教训）
- 无任何 TcpClient/Connector 残留，全新组件

## 3. 已裁决设计决策（用户确认）

| # | 决策点 | 裁决 |
|---|---|---|
| 1 | 总体路线 | **方案 A 全栈一轮**：U0-U4 拆单元交互推进 |
| 2 | 构造接口 | **`TcpClient(EventLoop*, string_view "ip:port", name)`**——与 TcpServer 对称，非法抛 AddressError |
| 3 | 失败通知 | **muduo 式统一 conn_cb**：失败时构造短命 TCPConnection（Disconnected 态）触发 conn_cb；send 到失败连接被安全拒绝 |
| 4 | 文件组织 | **TcpClient.h/.cc 单文件包含 Connector**（循 EventLoopThread/Pool 先例） |

## 4. 处理单元设计

### U0 设计文档（本文）+ 地址解析公共化

- `TcpServer::ParseListenAddress`（私有静态）与 `detail::ParsePort` 迁移为 **Address 静态工厂 `FromIpPortString`**（语义归属地址构造）；TcpServer 改调（行为不变）；AddressTest 补解析用例

### U1 TCPConnection::connectDestroyed（客户端收尾路径）

- loop 线程内 `Connecting/Connected → Disconnected` + `DisableAllEvent()` + **前置终态后**触发 conn_cb（循 D38 次序纪律：回调后零成员访问）
- 分工：服务端关闭走 handleClose（fd 事件触发）；客户端连接失败/中断无事件可触发，由 TcpClient 显式调用

### U2 Connector（TcpClient.h 内）

- 状态机 kDisconnected/kConnecting/kConnected（atomic）
- Start（loop 线程）：NIO_TCP socket + `::connect`：0 → 直接成功；EINPROGRESS → EnableWriteEvent；ECONNREFUSED/EADDRINUSE/EAGAIN 等临时错误 → Retry；其他 → 失败回调
- handleWrite：先 DisableAllEvent → getsockopt(SO_ERROR)：0 → NewConnectionCallback(fd)（fd 移交，**channel 不销毁**——D27/D39 纪律，复位延至下次 Start 或分派轮外）；非 0 → Retry / 失败回调
- Retry：退避 = min(500ms << retry_count, 30s)，经自持 TimerManager（loop 线程构造/析构）一次性 addTimer 到点 restart；Stop 在 loop 内 cancel timer 并收尾
- Retry/Stop 竞态协议：Stop 时序保证 timer 闭包不悬垂（实施时细化 + 测试锁定）

### U3 TcpClient 本体

- ctor(loop, "ip:port", name)：解析失败抛 AddressError；成员：unique_ptr\<Connector\>、`std::atomic<std::shared_ptr<TCPConnection>>`（C++20，支持跨线程 connection() 读）
- connect()/disconnect()（已连接则 conn->shutdown()）/stop()（取消重试）/enableRetry()
- 建连链（Connector 成功回调，loop 线程）：getLocalAddr/getRemoteAddr → TCPConnection（Connecting）→ setTcpNoDelay + 转发回调（close → removeConnection）→ 存储 → connectEstablished()
- 失败路径（Connector 失败回调，非 retry）：短命 TCPConnection（失败 fd）→ connectDestroyed() → conn_cb 收 Disconnected
- removeConnection（close_cb）：m_conn = nullptr；retry 且未主动 stop → Connector->Restart()（断线自动重连）
- 析构：Stop 收尾 + 契约断言；销毁全部在分派轮外（循 D39 协议）

### U4 TcpClientTest + 全矩阵终验

新测试目标（fixture：LoopThread + TcpServer 装配 echo——客户端对真实服务端栈对测）。用例：ConnectAndEcho / ServerCloseTriggersClientClose / ClientDisconnectHalfClose / ConnectRefusedNotifiesViaConnCb / RetryReconnectsAfterServerStarts / StopCancelsRetry。

终验：40 目标（39+1）× {常规, TSAN(die_after_fork=0), ASAN} 矩阵 + TcpClientTest repeat:10 + 收账。

## 5. 交互式处理协议（每处理单元一轮）

① 问题卡 → ② 方案确认（AskUserQuestion）→ ③ 代码确认（before/after 完整展示，逐项确认后才动工）→ ④ 实施（TDD 先红后绿）→ ⑤ 验证（ctest + 按性质追加 TSAN/ASAN）→ ⑥ 登记与摘要

## 6. 验证策略

| 时机 | 动作 |
|---|---|
| 每单元 | `ctest -R <受影响目标>`（build/ 常规） |
| 并发类（U2/U3/U4） | `TSAN_OPTIONS=die_after_fork=0 setarch $(uname -m) -R ctest --test-dir build-tsan -R <目标>` |
| 内存类（U1/U3） | ASAN 对应目标 |
| 终局 | 40×{常规,TSAN,ASAN} 矩阵 + repeat |
| 变体切换 | `rm bin/*Test lib/libcodebus.so && cmake --build <dir>` |

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| TimerManager 生命周期（构造/析构须 loop 线程） | U2 内 loop 线程管理（D6/D18 纪律；TimerTest ManagerGuard 先例） |
| retry 定时器闭包与 Stop/析构竞态 | Stop 在 loop 内 cancel + 收尾协议；StopCancelsRetry 锁定 |
| 非阻塞 connect 的 errno 分类边界 | muduo Connector 三档分类（EINPROGRESS/临时/致命），用例覆盖 ECONNREFUSED |
| Connector channel 移交 fd 后的摘除时序（D27/D39） | 成功路径只 DisableAllEvent；销毁延至分派轮外 |
| 短命失败连接的 fd 语义 | connectDestroyed 前不 EnableReadEvent；send 被状态机守卫拒绝 |

## 8. 进度记录

| 处理单元 | 状态 | 备注 |
|---|---|---|
| U0 设计文档 + 解析公共化 | **完成** | 2026-10-23：spec 落盘；`Address::FromIpPortString` 静态工厂（逻辑自 TcpServer 逐字迁移，错误前缀改为 "ip:port parse error"）；TcpServer 私有解析与 detail::ParsePort 删除改调（行为零变化，InvalidListenAddressThrows 继续锁定）；AddressTest 新增正反用例（TDD 编译红→绿）；常规 39/39 绿 |
| U1 connectDestroyed | **完成** | 2026-10-23：客户端失败/中断收尾路径（Connecting/Connected/Disconnecting→摘事件+终态前置+conn_cb 终态通知；Disconnected 幂等早退）；ConnectDestroyedFinalizesAndNotifies（TDD 编译红→绿：终态观察/恰一次/幂等）；常规 39/39 + ASAN TCPConnectionTest 绿 |
| U2 Connector | **完成** | 2026-10-23：**用户详询 fd 方案后裁决维持裸 socket_t**（所有权移交边界不经 Socket——无 release 出口且析构契约禁止 close 后析构；muduo 同款边界模式；~Connector 断言 m_fd<0 代位 RAII）；三档 errno 分类（{0,EINPROGRESS,EINTR,EISCONN}→connecting 统一 SO_ERROR 判定 / 临时错误→退避 / 致命→FailedCallback）+ 退避 500ms×2 封顶 30s + 惰性 TimerManager（未启用 retry 零 timerfd）+ weak_ptr 定时闭包 + D39 摘除纪律；实施中修正 TimerManager 构造参数（View→Peer() 的 reference_wrapper）；ConnectorTest 4 用例（TDD 编译红→绿：成功移交/被拒失败恰一次/退避期 Stop 取消以「事后起监听观察无重连」验证/未启动即停安全）；常规 40/40 + TSAN ConnectorTest 绿 |
| U3 TcpClient 本体 | **完成** | 2026-10-23：单连接模型（atomic\<shared_ptr\<TCPConnection\>\>，C++20）；建连链 getsockname/getpeername 同 TcpServer 模式 + setTcpNoDelay + 回调转发 + connectEstablished；失败路径短命连接 connectDestroyed 统一通知；removeConnection 清指针 + retry 且未 stop → Connector::Restart（断线自动重连）；stop 单向生命周期（对称 TcpServer I3）；析构双断言契约；**装配时修订 Connector（用户确认）**：FailedCallback 改 `void(socket_t)` fd 移交（短命连接需真实 fd，RAII 由接收方关闭；socket() 创建失败传 -1 且上层容忍跳过通知）；失败路径收敛为 failOrRetry/failNoRetry 两助手（致命不重试，muduo 语义）；实施中修正 getsockname 三元式参数列表笔误一处；ConnectorTest 改签名后常规 40/40 + TSAN 绿。TcpClient 行为验证归 U4 集成用例 |
| U4 TcpClientTest + 终验 | **完成** | 2026-10-23：6 用例新目标（对 TcpServer 真实服务端栈对测，含析构契约收尾）；**实施中发现并修复两项（均经用户确认）**：①handleClose 补 conn_cb(Disconnected) 断开通知（muduo 同序 conn_cb→close_cb；strace 定位建连/echo 正常而关闭通知落空——TcpClient 契约的「关闭」分支依赖此语义，既有用例经核实兼容）；②Connector Channel 回调改捕裸 this（shared_from_this 造成 Connector→Channel→回调引用环，ASAN 泄漏 1056B 实证；muduo 同款裸 this + ~Connector 补「Channel 已摘除」断言）；**三矩阵终验：41 目标 × {常规, TSAN(die_after_fork=0), ASAN} 全绿** + TcpClientTest/ConnectorTest repeat:10；CLAUDE.md（41 目标清单/待实现移除 TcpClient）与 README（TCP 栈完整可用）更新 |
| U5 审查修复轮（code-reviewer） | **完成** | 2026-10-23：审查裁决 **NO-GO**（Critical×1 + Important×1 + Minor×4 + INFO×4；不变量 A-H 主链 VERIFIED，引用环/B 的破绽仅存回调内 delete 场景）；用户裁决逐条交互修复。**#1 已完成（登记 D40 登记即闭环）**：Connector::Stop 内联路径的定时器分派帧内自毁 UAF（retry+致命 errno+回调内 stop 链）——Stop 统一 queue-only（C1 纪律对齐）；ConnectRefusedFiresFailed 加强（回调内 Stop 契约 + fd>=0 移交断言）；致命 errno 帧内精确复现需环境构造，按结构性修复+回归验证如实登记；常规 41/41 + ASAN 双目标绿。**#2 已完成**：Start CAS 失败 LOG_WARN + 头文件两处「单连接生命周期/重连须 retry」契约（行为零变化）。**#3+#4 已完成**：newConnection 入口 m_stopped 确定性拒绝（竞态窗口收编）；Restart 在途态设防（关旧 fd+摘事件——公共面防御，经 TcpClient 不可达）。**#5 已完成**：~TcpClient/~Connector 契约改 LOG_FATAL+SystemUtil::Abort（release 违约可观测，EventLoop 先例；assert 由 fatal 检查取代）+ 回调约束文档（回调内只允许 stop）。**INFO 批已完成**：#6 getsockname 失败改走统一失败通知（fd 由短命连接 RAII 关闭）；#7 ENOTCONN 补进临时错误表（muduo 对齐）；#8 Connector 须 make_shared 前置条件文档；#9 fd>=0 断言已随 #1 落地，致命 errno 用例经环境探针（255.255.255.255→ENETUNREACH 属临时表）确认不可确定性构造——登记不造。收官终验：41 目标 × {常规, TSAN, ASAN} 三矩阵全绿 + TcpClient/Connector repeat:10。**U5 审查修复轮全部闭环** |
