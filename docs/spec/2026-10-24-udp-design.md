# UDP 模块设计与计划（全栈一轮 · 交互式）

> 状态：实施中（V0/W0/W1 完成；W2 待 Server 验收）
> 关联：TcpServer/TcpClient 两轮已闭环（[2026-10-22](2026-10-22-tcp-server-design.md)、[2026-10-23](2026-10-23-tcp-client-design.md)）；本轮为待实现清单最后一项网络协议

## 1. 需求背景

交付达到开源网络库基准（asio/libuv 对照）的 UDP 模块。muduo 无 UDP 先例——参考面：asio（双模式 + 多播）、sylar（UdpServer）。现有基础：`Socket::Type` 已含 UDP/NIO_UDP（`detail::CreateSocket` 支持 SOCK_DGRAM），但无 recvfrom/sendTo 原语与上层组件。

**用户要求**：六步交互协议逐单元推进（方案与代码确认后动工）。

## 2. 已裁决设计决策（用户确认）

| # | 决策点 | 裁决 |
|---|---|---|
| 1 | API 形态 | **对称双类**：UdpServer（bind+回调收发）+ UdpClient（可选 bind/可选 connect），对齐 Tcp 家族；**单文件 src/net/Udp.h/.cc** |
| 2 | 广播/多播 | **纳入本轮**：SO_BROADCAST + join/leaveGroup + TTL（Socket 层 + 组件暴露 + 用例） |
| 3 | UdpClient 模式 | **双模式**：可选 connect()（连接型：仅收对端、send 走 write）；自由型 sendTo 任意地址 |

## 3. 复用的既有资产与纪律

- Channel 读事件 + **循环排空纪律**（Listener accept 循环同款）
- 构造期 bind 失败抛 SocketError（I1）；Stop queue-only（D39）；析构契约断言 + release LOG_FATAL（TcpClient 审查 #5 先例）
- NIO_UDP 非阻塞 fd 适配事件驱动；**UDP sendTo 数据报内核原子——任意线程安全**
- 单 loop 线程模型（UDP 无连接分发；多线程吞吐经 SO_REUSEPORT 多实例，文档化）

## 4. 处理单元设计

### V0 spec（本文）+ Socket 层 UDP 原语

[src/net/Socket.h/.cc](../../src/net/Socket.h)：`recvFrom`（返回 {字节, 对端}，family 判定同 accept 模式）、`sendTo`、`setBroadcast`、`joinGroup/leaveGroup`（IPv4 IP_ADD/DROP_MEMBERSHIP，INADDR_ANY 接口；IPv6 组播返回 false + 日志，文档注明延后）、`setMulticastTTL`。SocketTest 补原语/选项用例（joinGroup 环境依赖部分容错）。

### V1 UdpServer

- ctor(loop, "ip:port")：构造期 NIO_UDP socket + bind（失败抛 SocketError）；组播场景 bind 0.0.0.0:port + joinGroup(addr)（文档化）
- Start 幂等；Stop queue-only + 监听语义（Listener::Stop 同款）；析构契约 + LOG_FATAL
- handleRead 循环 recvFrom 排空（64KB 栈缓冲）→ `MessageCallback(Slice data, const Address& peer)`
- sendTo(data, peer) 任意线程；Stop 后拒绝；选项转发

### V2 UdpClient

- ctor(loop) 自由型（sendto 自动 bind 临时端口）；connect(addr) 进连接型（仅收对端、send 走 write、ICMP 不可达在收路径报错）
- Start/Stop 对称 TcpClient（单向生命周期、析构契约）；连接型混用 sendTo 的 Linux EISCONN 语义文档化

### V3 UdpTest 用例矩阵（目标数 40→41）+ SocketTest 扩展

EchoRoundTrip / MultiClientInterleave / MaxDatagram（近 64KB）/ ConnectedModeFilters（内核过滤锁定）/ Broadcast（选项必断言、收发容错）/ Multicast（lo + IP_MULTICAST_LOOP 本机可测）/ IcmpUnreachable（ECONNREFUSED 收路径不崩可恢复）/ StopRejectsSend + 析构契约。

### V4 终验 + 收账

41 目标 × 三矩阵 + UdpTest repeat:10 + README/CLAUDE.md/known-issues/spec 收账。

## 5. 交互式处理协议（每单元一轮）

① 问题卡 → ② 方案确认 → ③ 代码确认（before/after，确认后动工）→ ④ 实施（TDD）→ ⑤ 验证（ctest + 按性质 TSAN/ASAN）→ ⑥ 登记与摘要

## 6. 验证策略

| 时机 | 动作 |
|---|---|
| 每单元 | `ctest -R <目标>`（build/ 常规） |
| 并发类（V1-V3） | `TSAN_OPTIONS=die_after_fork=0 setarch $(uname -m) -R ctest --test-dir build-tsan -R <目标>` |
| 终局 | 41×{常规,TSAN,ASAN} 矩阵 + repeat |
| 变体切换 | `rm bin/*Test lib/libcodebus.so && cmake --build <dir>` |

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 广播发送环境依赖（本环境 255.255.255.255 已实证 ENETUNREACH） | 选项置位必断言；定向子网广播收发容错 |
| 组播环境依赖 | lo + IP_MULTICAST_LOOP（默认开）本机可测；失败环境 SKIP |
| 读排空/EAGAIN 语义 | NIO_UDP + Listener 排空纪律；积压用例覆盖 |
| connected UDP 混用 sendTo | 文档化 Linux 语义；过滤行为用例锁定 |
| 回调内 sendTo 重入 | 数据报原子 + 同 loop 串行（设计保证） |
| ICMP 不可达异步时序 | 收路径错误分类；用例容错等待 |

## 8. 进度记录

| 处理单元 | 状态 | 备注 |
|---|---|---|
| V0 spec + Socket 原语 | **完成** | 2026-10-24：recvFrom/sendTo（MSG_DONTWAIT 单调用语义，family 判定同 accept）+ setBroadcast/joinGroup/leaveGroup/setMulticastTTL（IPv4；IPv6 返回 false 延后）；SocketTest 新增 2 用例（TDD 编译红→绿；EAGAIN 排空语义 + getsockopt 复核 + joinGroup 环境容错 SKIP）；**实施中处置两项**：①发现并修复已提交的编译回归——用户 46bc2d3 给 MacroReflaction.hpp 包 namespace bus{，破坏「宏驱动多重包含模板头」（类落入 bus::bus），回退包裹并补防再犯注释（编译失败的 ctest 表现为旧二进制假绿，循共享输出纪律强制重链定位）；②测试初次用 Address(port:0) 撞 I2 的「port 0 非法」断言——改用 GetFreePort（Address 语义不动，临时端口场景避开争议）；常规 40/40 绿 |
| V1 UdpServer | **完成** | 2026-10-24：**用户裁决 UdpServer/UdpClient 分立文件**（覆盖原同文件方案）——src/net/UdpServer.h/.cc；构造期 bind 抛 SocketError + getsockname 实际地址 + setReuseAddr；Start 幂等；Stop queue-only（仅摘事件，**fd 延迟至析构关闭**——避免任意线程 sendTo 撞 Socket::sendTo 断言的竞态窗口）；**析构自动收尾**（未 Stop 则 Stop + future 等 loop 侧完成；回调内析构检测 LOG_FATAL——EventLoopThread 先例）；64KB 排空（EAGAIN/EINTR/错误分类；回调内 Stop 即止；Slice 仅回调内契约）；实施中修正 4 处编译错（陈旧 include 路径×2、缺 Buffer.hpp/Socket.h、throw 表达式笔误、测试 sendTo 旧签名）；UdpTest 新目标 3 用例（TDD 编译红→绿：收发回显/Stop 拒绝/未 Stop 析构安全）；常规 41/41 + TSAN UdpTest 绿 |
| — 方向反转（用户两轮裁决） | **完成** | 2026-10-24/25：①用户指出事件驱动形态下「事件分配」未完成（TCP 有 EventLoopThreadPool 分发连接，UDP 单 socket 全落一个 loop）→ 先拟同步模型；②复议后指示继续事件驱动并补齐分配（SO_REUSEPORT 多端点方案已呈确认）→ 最终裁决**放弃事件驱动（过于复杂），改同步模型：UDP 主线程 + 任务线程池**。W0 已执行：删除事件驱动 UdpServer.h/.cc 与 UdpTest 3 用例（用户确认；V0 Socket 原语保留为同步模型地基）；常规 40/40 绿。原 V2-V4 计划作废，重排为 W1-W3 |
| W1 同步 UdpServer（主线程+任务池） | **完成** | 2026-10-25（**用户复审修订：构造即启动**——接收线程与线程池移入构造函数，Start() 移除；回调改为可随构造传入（消除首批报文与构造后设置间的竞态窗口），ctor 增补 cb 参数与线程创建失败的异常安全回收；用户手动补 setKeepAlive/注释行保留）：无 EventLoop 依赖；ctor(addr, worker_num=1, cb)（bind 失败抛 SocketError + getsockname）；接收主线程 poll(200ms)+停止检查 → 排空 recvFrom（64KB）→ 任务{数据拷贝,对端}入池；ThreadPool 复用（stop 排空语义由 D35 修复保证，用户裁决排空）；MessageCallback(string_view, peer) 于工作者线程（数据任务自有——无排空缓冲限制）；跨报文仅 worker_num=1 保序（FIFO+单工作者）；sendTo 任意线程/Stop 后拒绝；析构自动收尾（回调内禁止析构——死锁，文档契约）；UdpTest 5 用例（TDD 编译红→绿：回显/并行总数/单工保序/Stop 排空/析构安全）；常规 41/41 + TSAN 绿；StopDrains 用例提交裕度 200→500ms（饱和负载下时序滑动加固，TSAN repeat:5 稳定） |
| W1.1 修订：改回显式 Start（用户两步裁决） | **完成** | 2026-10-26：①用户裁决改回两段式（原「构造即启动」为用户前次复审修订，本次反转）；②细化裁决「**接收线程与任务池的创建在构造函数里，启动在 Start 里**」——构造仅 bind + 创建 Thread/ThreadPool 对象（就绪不运行），Start() 启动（幂等 CAS；Stop 后拒绝——单向生命周期；启动失败回收已启动部分并重建未启动对象，可重试——I4 纪律）；setMessageCallback 前置契约（Start 后忽略并警告，std::function 非原子）；未 Start 析构安全（m_started 跳过自动收尾）；UdpTest 4 用例补 Start() + DestructWithoutStopIsSafe 改两段式 + 新增 DestructWithoutStartIsSafe（TDD 编译红→绿）；常规 41/41 + TSAN UdpTest + repeat:5 全绿；另：验收基线复验时 TSAN StopDrains 首跑 1 次时序滑动（满负载），复跑 5 连绿——裕度偏紧登记为观察项 |
| W1.2 修订：MessageCallback 的 data 改 Buffer* | **完成** | 2026-10-26：用户裁决「data 采用 Buffer 类型」并进一步指定**指针形态**（`void(Buffer* data, const Address& peer)`——与 TCP 侧 MessageCallback 的 Buffer* 一致）；recvLoop 按报文实长构造 Buffer 入任务（回调借指针零拷贝视图、可 std::move(*data) 接管）；实施中修正：任务闭包经 ThreadPool::run 的 const 通路使捕获成 const——lambda 补 mutable（编译错定位，首跑 ctest 假绿系旧二进制——共享输出陷阱再次应验，删 bin/UdpTest 重链后真验）；5 用例签名适配；常规 41/41 + TSAN UdpTest（首跑时序滑动同观察项，复跑 5 连绿） |
| W1.3 修订：datagram 改 shared_ptr 持有 | **完成** | 2026-10-26：用户裁决「使用智能指针，不然存在悬垂的问题」——**如实澄清**：原 init-capture 已将 Buffer 移动进闭包（&data 指向闭包自有对象，无悬垂），但 shared_ptr 使所有权在调用点显式化、对未来重构健壮，按裁决实施：make_shared<Buffer> → 闭包共享持有 → m_msg_cb(data.get(), peer)（回调签名不变，mutable 不再需要）；**连带发现并修复测试缺陷**：TSAN 3/3 稳定复现 SingleWorkerPreservesOrder 乱序（order[0]=9/order[1]=16）——根因为 W1.2 适配时 sscanf 改读 Buffer 无 NUL 终止区，越过 ReadableSize 吃进容量区堆垃圾数字（旧 std::string 自带 NUL 故未暴露；常规构建垃圾恰为 0 故假绿；TSAN 堆布局暴露）——修为按 ReadableSize 拷贝栈缓冲补 NUL 再解析，并在用例中文档化「Buffer 无 NUL 终止、消费须界定」契约；排障过程三次踩共享输出陷阱（误验常规二进制为 TSAN 证据），已按变体切换纪律重验；常规 41/41 + TSAN UdpTest 直跑 3 连 + ctest 绿 |
| W1R 审查修复轮（code-reviewer UdpServer） | **完成** | 2026-10-26：审查裁决 **NO-GO**（Critical×1 + Important×4 + Minor×8 + INFO×7）。**#1 已完成**：回调签名三方不一致致不可编译——根因为用户手动改签名（shared_ptr 形态）后测试未对齐（时间戳证据证伪此前「41/41 绿」）；用户裁决**保留 shared_ptr**，UdpTest 5 处 lambda 对齐 + 头文件注释改写 + Buffer.hpp include 适配（用户头文件改前置声明的 hygiene），强重链真验。**#3 已完成**：未 Start 先 Stop 的 join 崩溃——Stop 开头未启动守卫 + StopWithoutStartIsSafe 用例。**#4 已完成**：回调内 Stop 契约禁令（文件头 + Stop 声明两处：self-join EDEADLK→terminate 链）。**#2 已完成（D41 闭环，三轮交互）**：ThreadPool 排空缺失——用户先裁决「增加处理完成所有队列后再停止的函数」并指向 BlockingQueue::drain；初版快照方案（drain 搬出+调用线程执行）被用户否决（卡主线程，残余须在池内跑）；终版「threadFunc 原循环零改动 + 条件排空段」（drain() 不适用的原因已说明：swap 搬出与池内执行矛盾）；take() over 后不阻塞的疑虑经谓词代码确认；stopAndDrain 落地 + UdpServer::Stop 接入 + Stop 文档恢复排空声明；排障连带：run() 前置守卫系用户手动改动（二分+插桩定位，非 D41 引入）——用户裁决保留并翻转 RunWithoutStart 用例；二分残留补丁教训登记。**Minor 批已完成（M5-M12）**：#elif→#else / setReusePort 转发暴露（含 UDP 投递语义注释）/ POLLERR 消费防忙转 / 构造单次解析 / isAvailable 兜底（Socket 层缺陷登记 Q32）/ sendTo 失败日志 / port 断言 / 行尾 CRLF→LF 不回退（建议单独规范化 commit）。验证：常规 41/41 + TSAN UdpTest + repeat:5 全绿（严格变体纪律）。搁置：Start 失败回收零测试（需线程耗尽环境）、回调异常 terminate（项目级行为）、IPv6/截断/广播多播未覆盖（W3） |
| W2 同步 UdpClient | — | Server 验收后设计（用户指定顺序） |
| W3 用例矩阵扩展 + 终验收账 | — | 广播/多播/连接型用例与文档收账 |
| V2 UdpClient | — | |
| V3 UdpTest 矩阵 | — | |
| V4 终验 + 收账 | — | |
