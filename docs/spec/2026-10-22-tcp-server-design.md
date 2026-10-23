# TcpServer 完善设计与计划（全栈一轮 · 交互式）

> 状态：**已完成**（P0–P8 全部闭环，2026-10-22）
> 关联：[known-issues.md](../known-issues.md)（D27/Q30/D38 已闭环；D8 延后）、[2026-10-19-defect-fix-plan.md](2026-10-19-defect-fix-plan.md)（前置质量基础）

## 1. 需求背景

TcpServer 为空壳（[src/net/TcpServer.cc](../../src/net/TcpServer.cc) 仅构造函数存字段，thread_num 被忽略），EventLoopThreadPool 为空类，EventLoopThread 不存在；测试侧 EchoServer 用裸 Channel/Socket 手拼并以 m_zombies 绕 D27。本任务打通完整服务端栈，产出**完备的 TCP server**（对照 muduo：accept → 建连 → 连接管理 → 多 IO 线程分派 → 优雅关闭），同时闭环 D27/Q30 并回收测试绕行。

**用户要求**（沿用缺陷修复轮协议）：单次一个处理单元、交互式；解决方案与具体代码确认后方可改动；完成一项再进入下一项。

## 2. 现状盘点

**已具备**（缺陷修复轮 P1–P26 后）：EventLoop（RunInLoop/QueueInLoop/wakeup/Stop）、Channel、Listener（bind/listen/accept → NewConnectionCallback）、TCPConnection（connectEstablished/send 族/handleClose/高水位）、Socket、Thread、ServerStatus。

**空壳/缺失**：TcpServer 本体；EventLoopThreadPool（空类，未入编译清单）；EventLoopThread（无文件）。

**挡路的隐患**：
- **D27**：EventLoop 分派循环（[EventLoop.cc:87-98](../../src/net/EventLoop.cc#L87-L98)）对 activeChannel 裸指针——回调链内析构 Channel 即 UAF（TSAN 已复现）。TcpServer 的 removeConnection 是关闭必经链路，**D27 是前置**
- **D38（本轮新登记）**：handleClose 在 close_cb（未来的 removeConnection）返回**后**还写 m_status、打日志（[TCPConnection.cc:114-119](../../src/net/TCPConnection.cc#L114-L119)）——close_cb 若就地销毁连接即 UAF
- **Q30**：m_status 跨线程非原子读（send 路径）
- Listener：单次 accept、EMFILE 仅记日志（fd 耗尽 busy-loop 风险）
- Address 无 "ip:port" 解析，而 TcpServer 构造收 string_view

## 3. 已裁决设计决策（用户确认）

| # | 决策点 | 裁决 |
|---|---|---|
| 1 | 总体路线 | **方案 A 全栈一轮**：拆 P1–P8 处理单元逐个交互推进 |
| 2 | D27 修法 | **tie/weak_ptr**（muduo 式存活守卫；代码骨架已展示确认） |
| 3 | 构造接口 | **保持 `TcpServer(EventLoop*, string_view "ip:port", thread_num)`**，内部解析（IPv4 `ip:port` / IPv6 `[x::y]:port`，失败抛异常） |
| 4 | Stop 语义 | **全停 + 完成回调**：摘 Listener → 逐连接 handleClose 链 → 表空后 DoneCallback 在 base loop 触发一次（幂等） |

## 4. 处理单元设计

### P1 D27：Channel tie/weak_ptr 存活守卫

三处源码修改：

1. **Channel.h**：新增 `tie(const std::shared_ptr<void>&)` + `m_tied`/`m_tie`（weak_ptr）。约束：须在 Enable*Event 之前、loop 线程调用（此后与 HandleEvent 同线程，无锁）
2. **Channel.cc HandleEvent** 开头守卫：`m_tied` 时 `m_tie.lock()`——提升失败事件作废（防御分支）；成功则分派期间宿主引用计数 ≥1
3. **TCPConnection::connectEstablished**：`EnableReadEvent()` 前 `m_channel->tie(shared_from_this())`

**保护范围语义**（重要，实施时向用户确认）：
- ✅ **自身分派栈内销毁**：宿主在自己的 HandleEvent 回调链里被 erase/reset（handleClose → removeConnection、EchoServer CloseConnection）——守卫持引用，析构推迟到 HandleEvent 返回
- ✅ **分派轮外销毁**：pending functor 持有的引用在 doPendingFunctor（activeChannel 循环之后）释放
- ❌ **不支持**「在 A 的回调里清零 B 的全部 shared_ptr 引用」——B 未在分派中，无守卫；muduo 同样不支持。合法关闭 B 的路径是触发 B 的事件链（B 自身分派栈内）或经 loop 排队

配套测试（TDD 先红）：
- `ChannelTest::DestroyDuringDispatch` 改写为宿主形态（Host 持 Channel + tie）：连接表 erase 自销毁路径，断言「回调内宿主未析构（守卫生效直接证据）+ 分派结束后析构」
- 新增 `TieExpiredEventDropped`：anchor 先亡 → HandleEvent 事件作废不回调（防御分支直测）
- **EchoTestHelper**：OnAccept 补 `channel->tie(conn)`；CloseConnection 直接 erase（真实销毁）；删 m_zombies 列表；known-issues D27 标记闭环

### P2 EventLoopThread（新建 src/net/EventLoopThread.h/.cc）

参照已验证的 [test/EventLoopTestHelper.h LoopThread](../../test/EventLoopTestHelper.h) 语义生产化（基于项目 Thread 封装）：
- `EventLoopThread(ThreadInitCallback, name)`；`EventLoop* StartLoop()`（latch 同步等就绪）；析构 = Stop + 放行 + join（m_may_finish 协议防析构/Stop 竞态——LoopThread 已验证此模式，直接移植）
- 线程函数：构造 EventLoop → initCb → `Loop()` → 放行后析构 loop
- 接入 src/CMakeLists.txt；新测试目标 EventLoopThreadTest（test/net/）

### P3 EventLoopThreadPool（填充空壳）

- base loop + N 个 EventLoopThread；`Start(initCb)` 幂等；`getNextLoop()` round-robin（thread_num=0 返回 base）；`getBaseLoop()`；析构由 EventLoopThread 析构收尾
- 新测试目标 EventLoopThreadPoolTest（round-robin 分布、Start 幂等、析构收尾）

### P4 TCPConnection 加固

- **D38（新登记即修）**：`m_status = Disconnected` 移到 invoke(m_close_cb) 之前，删尾部重复日志——回调可观察终态且回调后无 this 访问
- **Q30 闭环**：`m_status` 改 `std::atomic<Status>`；TCPConnectionTest PeerClose 用例的 RunInLoopSync 规避注释回收评估
- **新增 `shutdown()`**：优雅半关闭——置 Disconnecting；RunInLoop 内 output_buffer 空则 `Socket::ShutdownWrite()`；handleWrite 清空输出时若已 Disconnecting 补 ShutdownWrite
- 顺带：handleRead/handleWrite 的 EAGAIN 分支 `LOG_ERROR` → `LOG_TRACE`
- 测试：Shutdown 半关闭用例；TSAN 验证

### P5 Listener 加固

- **EMFILE 预留 fd**（idleFd）：构造 `::open("/dev/null")`；accept 失败 EMFILE 时 close idle → accept（拒绝）→ close → 重开 idle
- handleRead 改循环 accept 至 EAGAIN
- 测试：ListenerTest；EMFILE 模拟用例（rlimit 收缩）复杂度实施时裁决

### P6 TcpServer 本体（单/多 loop 一步到位）

> **注**：本节为原始设计；实施与 P9 审查修订后的**实际形态**见下方「设计→实现偏离」及源码。

- **构造**：内部解析 `"ip:port"` → Address（失败抛 Exception）；成员：`unique_ptr<Listener>`、`ConnectionMap = unordered_map<string, TCPConnection::ptr>`、`atomic<bool> m_started`、`atomic<uint32_t> m_next_conn_id`
- **Start()**：CAS 幂等；base loop RunInLoop：Listener 装配 + Listen；thread_num>0 启动线程池
- **建连链**：listener cb（base loop）→ ioLoop 选择（0=base 否则 getNextLoop）→ ioLoop RunInLoop：getLocalAddr、connName=`<name>-<ipPort>-<id>`、`setTcpNoDelay(true)`、转发 5 件套回调（close → removeConnection）、`connectEstablished()`
- **removeConnection**：conn 的 ioLoop 触发 → RunInLoop 回 base loop map erase（单 loop 模式立即执行，D27 守卫兜底析构安全）；表空且 stopping → DoneCallback
- **Stop(cb)**：base loop RunInLoop：Listener.reset() → 逐 conn `close()`（removeConnection 全部汇聚回 base loop）→ 表空后 DoneCallback 触发一次（幂等）
- **析构**：assert 已 Stop 且连接表空
- 新测试目标 TcpServerTest；Q15（DoubleClose 二次回调）到达时顺带裁决

**设计→实现偏离（P9 审查修订，M4 登记）**：

| # | 原设计 | 实际实现 | 原因 |
|---|---|---|---|
| 1 | 建连在 ioLoop 内完成（getLocalAddr/connName/回调装配） | **base loop 建连入表，仅 connectEstablished 投 ioLoop** | 连接表 m_conn_map 单线程（base loop）访问的前提（审查不变量 B）；muduo 同款线程归属 |
| 2 | Stop 经 base loop RunInLoop，`Listener.reset()` 摘监听 | **Stop 经 QueueInLoop 调度（分派轮外执行）+ Listener::Stop 暂停式（摘事件+shutdown 拒新连），对象延迟销毁至 ~TcpServer** | C1/D39：分派轮内销毁 Listener/连接 Channel 造成同轮悬垂 UAF（TDD 双红面实证） |
| 3 | Start 时装配 Listener | **TcpServer 构造期即建 Listener，bind 失败抛 SocketError** | I1/Q14：端口占用等若在 loop 线程内静默失败即幽灵启动，无 API 可感知；构造期同步抛给用户 |

### P7 EchoServer 迁移 + 多线程验证

- EchoTestHelper 的 EchoServer 重写为 TcpServer 装配（echo 语义不变），删手拼 Channel/Socket 代码
- NetworkStressTest 的 NO_LEAK_CHECK 评估解除（D27 闭环 + 连接生命周期归 TcpServer）
- README 模块交互图更新（TcpServer → EventLoopThreadPool → EventLoop）

### P8 终验 + 登记收尾

- 39 目标（36+3）常规全量 + 核心目标 TSAN/ASAN 矩阵 + `--repeat until-fail:10`
- known-issues：D27/Q30/D38 闭环标记；`grep -rhoE "TODO\(D[0-9]+" test/` 终态核对；本账本收尾

## 5. 交互式处理协议（每处理单元一轮）

① 问题卡（语义/现状/证据/波及面/方案）→ ② 方案确认（AskUserQuestion）→ ③ 代码确认（全部改动 before/after 展示，用户逐项确认后才动工）→ ④ 实施（TDD 先红后绿）→ ⑤ 验证（ctest + 按性质追加 TSAN/ASAN）→ ⑥ 登记与摘要（known-issues 标记、核对落地与确认一致）

## 6. 验证策略

| 时机 | 动作 |
|---|---|
| 每单元 | `ctest -R <受影响目标>`（build/ 常规） |
| 并发类（P1/P2/P3/P6/P7） | TSAN 对应目标（`setarch $(uname -m) -R`） |
| 内存类（P1/P6/P7） | ASAN 对应目标 |
| 每 2-3 单元 | 全量 ctest |
| 终局 | 39×{常规,TSAN,ASAN} 矩阵 + repeat + TODO(Dxx) grep |
| 变体切换 | `rm bin/*Test lib/libcodebus.so && cmake --build <dir>`（共享输出踩坑，见 CLAUDE.md） |

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| EventLoopThread 析构与 Stop 竞态 | 移植 LoopThread 已验证的 promise 放行协议 |
| 单 loop 模式 removeConnection 立即执行 → 析构在分派栈内 | D27 守卫兜底（P1 先行）；TcpServerTest + TSAN 验证 |
| 多线程 Stop 汇聚 | removeConnection 全部经 RunInLoop 回 base loop 天然汇聚；DoneCallback 幂等 |
| "ip:port" 解析边界错 | 用例覆盖 IPv4/IPv6/[::1]/无端口/非法格式 |
| EchoServer 迁移动摇集成稳定性 | NetworkStressTest 全量 + repeat，迁移前后对拍 |
| Q15 在 TcpServer 下放大 | P6 到达时裁决（幂等化或文档化） |

## 8. 进度记录

| 处理单元 | 状态 | 备注 |
|---|---|---|
| P0 设计文档（本文） | 完成 | 2026-10-22 |
| P1 D27 tie/weak_ptr | **完成** | 2026-10-22：用户裁决 tie/weak_ptr；TDD 红面（ASAN heap-use-after-free @ Channel.cc:60 ← EventLoop.cc:94）；Channel tie+守卫、TCPConnection 登记锚点；DestroyDuringDispatch 改宿主形态启用 + TieExpiredEventDropped 新增；EchoTestHelper m_zombies 删除（即时销毁），**实施中发现并处置**：①HalfClose 断言系 D27 绕行行为锁定（用例注释自证），按三动作回收翻转为正确语义（回显后 EOF/FIN）；②新用例完成信号次序自竞态（erase 后再置位）TSAN 消除。常规 36/36 + ASAN/TSAN 三目标绿 + repeat:10 |
| P2 EventLoopThread | **完成** | 2026-10-22：LoopThread 协议生产化（StartLoop 同步就绪 + init 回调 + 析构自动 Stop/放行/join）；红面=无实现编译失败→实施中修正 m_stopped_future 未关联 promise（future_error）一处；EventLoopThreadTest 4 用例；常规 37/37 + TSAN 绿 |
| P3 EventLoopThreadPool | **完成** | 2026-10-22：用户裁决**并入 EventLoopThread.h/.cc 同文件**（不单独建文件）——空壳 EventLoopThreadPool.h/.cc 删除（用户确认）；Start 幂等/atomic round-robin/0 线程退化 base/析构自动收尾；红面=未声明编译失败→实施中补 <cassert> 一处；5 用例新目标；常规 38/38 + TSAN 双目标绿 |
| P4 TCPConnection 加固（D38/Q30/shutdown） | **完成** | 2026-10-22：Q30 TSAN 红面精确复现（close() 写 vs status() 读）→ atomic 化零报告；**D38 诚实修正**：主动路径用例意外通过——核实发现 P1 tie 守卫与 close() lambda 捕获双重遮蔽 UAF 窗口（公共 API 无红面可达），转为结构加固（终态前置 + 回调后零成员访问）+ 被动路径用例锁定不变量；shutdown 半关闭（排空后 ShutdownWrite，handleWrite 补关）+ 显式 close 收尾语义；EAGAIN 降噪两处；常规 38/38 + TSAN/ASAN TCPConnectionTest 绿 |
| P5 Listener 加固 | **完成** | 2026-10-22：handleRead 改循环 accept 至 EAGAIN（LT 单事件排空积压）+ idleFd EMFILE 确定性拒绝（构造 open /dev/null 预占名额）+ Socket::accept 的 EAGAIN 降噪 + 每连接 WARN 日志删除（用户确认）；新用例 EmfileReleasesIdleSlotAndRecovers（rlimit 收缩 + RAII 恢复 + 3 次逐紧重试）；**实施中修正**：CountOpenFds 把 /proc/self/fd 的 . / .. 目录项计入且偏差方向写反（+1 应为 -1），strace 揭示三次 accept4 均成功后修正（过滤点项、扣除统计 fd）；常规 38/38 + ASAN/TSAN ListenerTest 绿 |
| P6 TcpServer 本体 | **完成** | 2026-10-22：整文件实现（"ip:port" 解析抛 AddressError、Start CAS 幂等、建连链 base loop 建连入表 + ioLoop connectEstablished、removeConnection 回 base loop、Stop 快照逐连接 close + 表空 finishStop（pool reset join + DoneCallback 一次）、析构双断言契约）；Q15 复核结论：TcpServer 流程下二次 close 被 handleClose 的 Disconnected 早退兜底，维持登记；**实施中修正**：①头文件嵌套回调类型需完整定义（改直接 include）②AddressError 仅收 const char*（.c_str()）③Start 异步装配竞态——测试补启动屏障（RunInLoopSync 空任务 FIFO 排序）；TcpServerTest 6 用例；常规 39/39 + TSAN/ASAN TcpServerTest 绿 |
| P7 EchoServer 迁移 + 多线程验证 | **完成** | 2026-10-22：EchoServer 重写为 TcpServer 装配（约 210 行手拼 Channel/Socket 退役，契约签名不变，NetworkStressTest 用例零改动）；README 示例与实现签名核对一致（预期 API 落地）；**用户裁决：NO_LEAK_CHECK 暂不解除**——已留 ASAN 强制泄漏检测证据（detect_leaks=1 直跑 12/12 零泄漏报告），解除另轮单独裁决；README 状态行/CLAUDE.md 交互图/待实现清单/测试目标清单（36→39）更新；常规 39/39 + NetworkStress repeat:10 + TSAN/ASAN 绿 |
| P8 终验 + 登记收尾 | **完成** | 2026-10-22：**用户裁决解除 NO_LEAK_CHECK**（CMake 开关移除，宏注释更新为「当前无目标使用」）——ASAN 全量 39/39 泄漏检测全开绿；**首次全矩阵 TSAN 暴露既有问题**：EventLoopTest.FunctorExceptionEscapesLoopAndTerminates 为 fork 型死亡测试，TSAN 禁止 fork 子进程建线程（exit 66）——`TSAN_OPTIONS=die_after_fork=0` 后全量 39/39 绿（已写入 CLAUDE.md TSAN 段）；终态核对：test/ 仅剩 TODO(D8)/DISABLED_ArrayTable（用户裁决延后项）；39×{常规,TSAN,ASAN} 三矩阵全绿。**用户随后按模块提交 4 个 commit（a6db39d 测试 → c088c9b EventLoopThread → 09103ff TcpServer → dec416c 文档）** |
| P9 审查修复轮（code-reviewer） | **完成** | 2026-10-22/23：审查裁决 GO-WITH-CHANGES（C1×1 + I1-I7 + M1-M5 + 搁置 9 项）；用户裁决逐条交互修复，全部闭环。**C1 已完成（登记 D39 登记即闭环）**：Listener::Stop 暂停式 + Channel kNone 快速路径 + Stop queue-only 调度（公开 EventLoop::QueueInLoop）——TDD 双红面（listener UAF 一次命中；A′ 不充分复跑揭示连接 Channel UAF）双修；ASAN 39/39 + 常规 39/39 + repeat:10 + TSAN 四目标绿。**I1 已完成（Q14 联动闭环）**：Listener 构造期 bind 失败抛 SocketError + TcpServer 构造期建 Listener（幽灵启动消除）+ listen 失败不启用事件；BindOccupiedPortIsSilent 翻转 + PortInUseThrowsAtConstruction 新增（双红面→绿）；常规 39/39 + ASAN 双目标绿。**I2 已完成（契约修正）**：Address 断言 `port<65535` 误排除合法上限——改 `port>0`，边界用例翻转为 65535、死亡测试仅留 0 越界（TDD 红面=65535 abort→绿）；常规 39/39。**I3 已完成**：startInLoop 开头 m_stopping 检查（Stop 后 Start 被忽略并记警告）+ Start 注释补单向生命周期契约 + StopThenStartIsIgnored 用例（红面=复活监听→绿）；常规 39/39。**I4 已完成**：StartLoop 捕获 Thread::run 异常回滚 m_started 后 rethrow（析构不再挂死）；红面=RLIMIT_NPROC 收缩触发 EAGAIN（timeout 探测 exit=124 永等）→绿；root 环境自动 SKIP；常规 39/39 + TSAN 绿。**I5 已完成**：StopClosesActiveConnections（3 活跃连接强制关闭：对端 FIN 观测/表空/Done 恰一次）——测试缺口补齐，路径为绿（C1 加固后预期），纳入确定性锁定；常规 39/39。**I6+I7/M5 已完成**：shutdown 注释修正（读路径不受影响，与实现对齐）；Socket::accept 与 EventLoop::WakeupRead 的 do-while 重试条件收紧（`connfd<0`/`n<0` 才检查 EINTR——残留 errno 不再诱发二次调用/fd 覆盖/误报）；该路径确定性触发需信号注入（审查未验证项#2），按回归验证（常规 39/39 + TSAN 39/39 全绿），不构造脆弱用例。**I 系列全部完成**。**M1 已完成（Q15 顺带闭环）**：用户逐条审阅采纳——初始态改 Connecting + close/shutdown 终态守卫 + 析构断言放宽双态（实施中连锁发现并处置）；DoubleClose 用例翻转（红面 2→1）；常规 39/39 + TSAN 双目标绿。**M2 已完成**：handleWrite 空缓冲早退（M2 注释在案；伪唤醒需注入 EPOLLOUT 伪事件不可确定性触发，无独立用例，按回归验证）。**M3 已完成**：finishStop 补 m_finished 幂等守卫——单 loop 同步链下最后一个连接 erase 触发收尾后 stopInLoop 尾部空表检查再次进入，重复 "stopped" 日志与 pool 空转 reset 消除（重复为外观性缺陷，DoneCallback 原有守卫不受影响，以现有 Stop 用例回归）。**M4 已完成**：P6 节补「设计→实现偏离」登记表（建连线程归属/Stop 调度与暂停式/构造期 Listener 三项及原因）。收尾验证：常规 39/39 + TSAN TcpServerTest 绿。搁置 9 项维持审查报告记录 |
