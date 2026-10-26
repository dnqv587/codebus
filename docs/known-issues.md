# 测试与已知缺陷

本文档是 codebus 测试体系的**缺陷登记活文档**：集中登记单元测试/集成测试/TSAN/ASAN 排查发现的全部已知问题，配套 [docs/spec/2026-10-19-unit-test-refactor-design.md](spec/2026-10-19-unit-test-refactor-design.md) 使用。

**登记规则**：

- 编号只增不改不复用；D 编号与测试代码中的 `TODO(Dxx)` 注释、`DISABLED_` 用例前缀一一对应，可 `grep -rhoE "TODO\(D[0-9]+.*\)" test/` 核对
- 每条缺陷的**启用条件** = 三动作：源码修复 + 去掉用例 `DISABLED_` 前缀 + 删除对应绕行代码，且 `ctest` 全绿
- 「现状锁定用例」是启用的用例，断言缺陷当前行为；源码修复后翻转为正向断言
- 测试只登记不修复源码；修复另立任务，完成后按三动作回收

---

## 1. 已知缺陷登记（历史迁移）

> 来源：test/ 旧测试文件中散落的 `DISABLED_` 用例与 `TODO(Dxx)` 注释，2026-10-19 测试体系重构时集中登记于此。旧用例名 → 新用例位置的迁移以设计文档 §8 阶段计划为准，迁移完成后更新本表「用例位置」列。

| 编号 | 缺陷语义 | 源码位置 | 用例位置（迁移后） | 绕行位置 | 启用条件 |
|---|---|---|---|---|---|
| D1 | ~~BoundBlockingQueue 构造条件写反（正容量即抛）+ drain() 不可编译~~ **2026-10-19 已闭环**（条件取反 `<=0` 抛；drain 改 `std::swap`——实施中发现第三处笔误：断言对象抄错（`assert(que.empty())` 应为 `m_queue`，不可编译时代从未暴露，已一并修正）；锁定用例翻转为非正容量拒绝 + 正向场景启用并补 drain 断言；36/36 绿） | src/base/BlockingQueue.hpp:94-120 | test/base/BlockingQueueTest.cc（ConstructorRejectsNonPositiveCapacity / BoundedPutTakeAndDrain 均已启用） | 无 | 已完成 |
| D2 | ~~Slice::operator\<=\> 比较 data 指针而非内容~~ **2026-10-19 已闭环（用户裁决：无消费者即删除）**——全库核查确认零比较使用（src 仅作传参类型、无序容器持有，test 唯一消费者为锁定用例自身），删除该运算符与 DISABLED_CompareByContent 用例；Slice 不再提供序比较，未来需要时按字典序重新设计 | src/Buffer.hpp（已删除） | 无（用例已删除） | 无 | 已完成 |
| D3 | ~~Buffer::Reserve 扩容未更新 m_capacity~~ **2026-10-19 已闭环**（resize 后补 m_capacity 同步；用例 ReserveUpdatesCapacity 已启用，Buffer/Slice/NetworkStress 全绿） | src/Buffer.hpp:202-207 | test/BufferTest.cc ReserveUpdatesCapacity（已启用） | 无 | 已完成 |
| D4 | ~~Condition::TimeWait 返回值语义反转（ETIMEDOUT 时返回 true）~~ **2026-10-19 已闭环**（`==` 改 `!=`，超时 false/唤醒 true；正向用例启用、重复锁定用例删除、唤醒半边翻正；thread 模块 7 目标绿） | src/thread/Condition.hpp:36 | test/thread/ConditionTest.cc（TimeWaitReturnsFalseOnTimeout / TimeWaitWokenByNotify 均已启用） | 无 | 已完成 |
| D5 | ~~ServerStatus::setStatus 声明返回 Status 但无 return（调用即 SIGILL）~~ **2026-10-19 已闭环**（方案 A：保存旧值返回，用户裁决；三个用例随启全绿；Q25 兜底一并修复） | src/net/ServerStatus.h:53-57 | test/net/ServerStatusTest.cc（三个用例均已启用） | 无 | 已完成 |
| D6 | ~~~TimerManager 为 default：不关 timerfd、不摘 Channel~~ **2026-10-19 已闭环**（析构 DisableAllEvent + ::close(m_fd)——经 UpdateChannel 的 kNone 分支摘除，避开 D18 的二次移除雷；TimerTest 的 CreateManager 泄漏绕行改为 ManagerGuard RAII（10 处，loop 线程构造/析构）；集成侧补 loop 线程 delete；NO_LEAK_CHECK 移除，ASAN 开泄漏检测绿；36/36 绿） | src/Timer.cc:182-190 | test/TimerTest.cc ManagerDestructorClosesTimerFd（已启用） | 无（已回收） | 已完成 |
| D7/D19 | ~~Timer::refresh 使用 system_clock 而非 steady_clock；先 cancel（异步队列）后 insert 自我删除~~ **2026-10-21 已闭环**（用户裁决采纳：steady_clock 改正（文件内全限定写法，与 insert 一致）；重排为「先按旧键同步 erase → 改到期时间 → 重插 → ResetTimer」——避开 set 键序破坏与 cancel 的异步 erase 自删，补 ResetTimer 对齐被推远的最早到期者；RefreshReArmsTimer 启用；36/36 绿） | src/Timer.cc:204-221 | test/TimerTest.cc RefreshReArmsTimer（已启用） | 无 | 已完成 |
| D8 | Value4Toml::asArrayTable 直接返回 nullopt | src/Config.cc:448-462 | test/ConfigTest.cc DISABLED_ArrayTable | 启用的 ArrayTableElementsReadableAsTables 走 asArray+asTable 替代路径 | 实现数组表解析 |
| D9/D11 | ~~send() 非 loop 线程按引用捕获 Slice（悬垂 UAF）~~ **2026-10-21 已闭环**（send 族重构为 queueSendData 公共排队路径：**数据拷贝入闭包**（值拷贝 Slice 仍悬垂——span 只持指针，须拷贝数据本身，muduo 同款语义）；三个 send 入口收敛；用例改写为 2048B 跨线程发送局部缓冲销毁后完整到达（含部分写→output buffer 路径）；ASAN 验证通过） | src/net/TCPConnection.cc（queueSendData） | test/net/TCPConnectionTest.cc SendFromNonLoopThreadIsSafe（已启用） | 无 | 已完成 |
| D10 | ~~send(Buffer\*) 为空实现（静默丢数据）~~ **2026-10-21 已闭环**（实现：loop 线程同步消费可读区（无生命周期窗口）/非 loop 线程拷贝入队后即消费——调用方 Buffer 在 send 返回后可复用（所有权转移语义）；用例 SendBuffer 启用并断言消费与到达；依赖 D11 先行解锁（用户裁决提前）） | src/net/TCPConnection.cc（send(Buffer*)） | test/net/TCPConnectionTest.cc SendBuffer（已启用） | 无 | 已完成 |
| D11 | ~~TCPConnection 缺 connectEstablished()，读路径不可达、无法进入 Connected~~ **2026-10-21 已闭环**（补 connectEstablished：置 Connected → EnableReadEvent → 触发 m_conn_cb（muduo 同序）；宽容幂等（非 Disconnected 记警告返回）；占位用例改写为完整回显链路用例 ConnectEstablishedEcho——建立回调/读路径激活/socketpair 回显/关闭回 Disconnected 全通；D9/D10 的验证路径就此解锁；36/36 绿） | src/net/TCPConnection.h/.cc（connectEstablished） | test/net/TCPConnectionTest.cc ConnectEstablishedEcho（已启用） | 集成侧 EchoServer 自建 echo 保留（TcpServer 装配前的独立实现） | 已完成 |
| D13 | ~~Buffer::WritableSize 断言 m_capacity > m_write_idx（恰好写满即 abort）~~ **2026-10-19 已闭环**（断言改 `>=`；ZeroCapacity 用例启用，两处随机模型「避开恰好写满」绕行删除——恰好写满路径纳入随机覆盖；EchoTestHelper 的 string 队列经用户裁决保留为实现选择，注释归因更新，待 D27 轮统一现代化；常规 36/36 + ASAN BufferTest 双绿） | src/Buffer.hpp:158 | test/BufferTest.cc ZeroCapacity（已启用） | 无（EchoTestHelper 侧转为实现选择） | 已完成 |
| D14 | ~~~EpollEventDispatcher 不关闭 m_EpollFd（每实例泄漏 1 fd）~~ **2026-10-19 已闭环**（析构 `::close(m_EpollFd)`，实现落 EventDispatcher.cc；用例 DestructorClosesEpollFd 启用——启用时发现迁移用例与 fixture 冲突（同线程二重 EventLoop 断言），用例补 `m_loop.reset()` 适配；集成侧 MultiLoopClientStorm 恢复 3 loops 原设计（用户裁决），容差 +2 归还；36/36 绿） | src/net/EventDispatcher.h:59 | test/net/EventDispatcherTest.cc DestructorClosesEpollFd（已启用） | 无（MultiLoopClientStorm 绕行已回收） | 已完成 |
| D15 | ~~getSocketErrno 用 setsockopt(SO_REUSEADDR) 探测而非 getsockopt(SO_ERROR)~~ **2026-10-19 已闭环**（改 `getsockopt(SO_ERROR)` 标准读法，副作用（误清 SO_REUSEADDR）一并消除；用例 GetSocketErrnoReportsSoError 启用，10 连跑稳定无时序 flaky） | src/net/Socket.cc:185-193 | test/net/SocketTest.cc GetSocketErrnoReportsSoError（已启用） | 无 | 已完成 |
| D16 | ~~setNonBlockAndCloseOnExec 经 F_SETFL 设置 FD_CLOEXEC（无效）~~ **2026-10-19 已闭环**（O_NONBLOCK 走 F_SETFL、FD_CLOEXEC 走 F_SETFD 分开设；用例启用，非阻塞半边既有用例保持绿） | src/net/Socket.cc:195-210 | test/net/SocketTest.cc SetNonBlockAndCloseOnExecSetsFdCloexec（已启用） | 无 | 已完成 |
| D17 | ~~Address::ipString 返回定长 NUL 填充字符串~~ **2026-10-19 已闭环**（v4/v6 两个 ToFormatIP 重载 return 前 `resize(strlen(c_str()))` 截断；用例 IpStringHasNoPadding 已启用，7 处 c_str() 绕行回收——含登记遗漏的 ListenerTest:72） | src/net/Address.cc:51-71 | test/net/AddressTest.cc IpStringHasNoPadding（已启用） | 无（已回收） | 已完成 |
| D18 | ~~~Listener 二次 RemoveChannel 触发断言（Debug 下无法安全销毁已 Listen 的 Listener）~~ **2026-10-20 已闭环**（用户裁决方案 A：析构仅留 DisableAllEvent（经 UpdateChannel 的 kNone 分支摘除）；UpdateChannel else 分支改 `else if(kNone 不 ADD)` 堵住「从未 Listen 的 channel 被僵尸 ADD 再手工拔除」的隐藏分支——两行互相抵消的路径整体消失；ListenerTest 六处泄漏回收（lambda 内 3 处 delete、lambda 外 3 处 RunInLoopSync delete）+ EchoTestHelper release()→reset() + NO_LEAK_CHECK 移除；ASAN 泄漏检测绿；36/36 绿） | src/net/Listener.cc:25-29 | test/net/ListenerTest.cc DestructorReleasesFd（已启用） | 无（已回收） | 已完成 |
| D20 | ~~inet_pton 返回值用 \<0 判断（失败返回 0，不抛 AddressError）~~ **2026-10-19 已闭环**（两处判定改 `!= 1`，非法 IP 显式抛 AddressError；用例 InvalidIpThrows 已启用，锁定用例 InvalidIpCurrentlyParsesAsAny 删除） | src/net/Address.cc:26,43 | test/net/AddressTest.cc InvalidIpThrows（已启用） | 无（已随锁定用例移除） | 已完成 |
| D21 | ~~StrToFileSize 不识别 "KB" 单位（返回 1）~~ **2026-10-19 已闭环**（补 `case 'K': return operator""_KB(size);`；Q19 越界读一并修复（循环首 `'\0'` 先行判定）；用例 StrToFileSizeKB 启用；注意唯一调用方 Logger 的 singleFileSize 配置语义随修复纠正（"4KB" 从 4 字节变 4096），与 D30 联动已记录） | src/util/Util.cc:15-47 | test/util/StringUtilTest.cc StrToFileSizeKB（已启用） | 无 | 已完成 |
| D22 | ~~Logger::logging 不使用 m_format~~ **2026-10-19 实测已不复现**（~Logging 经 m_format 格式化后提交，src/log/Logging.cc:42-54），疑似在历次修改中已修复；用例已启用作回归 | src/log/Logger.cc | test/log/LoggerTest.cc FormatterAppliedByLogger（已启用） | 无 | 已闭环，保留编号 |
| D23 | ~~BlockingQueue::take 用 T&& 绑定队首，pop 后返回悬垂引用（ThreadPool 全路径 UAF）~~ **2026-10-19 已闭环**（无界版 :241 与 Bound 版 :163 两处 `T&&`→`T` 值返回；MoveOnlyElements/SingleWorkerSerializesTasks/ConcurrentSubmitConservation 三用例启用；常规与 ASAN（重建 build-asan）双双 100% 绿——ASAN 确定性红面消除，证据链闭环） | src/base/BlockingQueue.hpp:241,163 | test/base/BlockingQueueTest.cc MoveOnlyElements（已启用）；test/thread/ThreadPoolTest.cc 两用例（已启用） | 无 | 已完成 |
| D24 | ~~Timestamp::toTimespec 的 tv_nsec 计算错误~~ **2026-10-19 已闭环**（修复：`tv_nsec = (m_us % 1e6) * 1000`；用例 TimespecNanoseconds 已启用，TimestampTest 全绿） | src/time/Timestamp.cc:40 | test/time/TimestampTest.cc TimespecNanoseconds（已启用） | 无 | 已完成 |
| O3 | ~~Address::toString 缺 ':' 分隔~~ **2026-10-19 已闭环**（与 D17 合并修复：`ipString() + ":" + to_string(port())`；用例 ToStringHasColon 已启用） | src/net/Address.cc:135-137 | test/net/AddressTest.cc ToStringHasColon（已启用） | 无 | 已完成 |

### 新登记（本次重构）

| 编号 | 缺陷语义 | 源码位置 | 用例位置 | 绕行位置 | 启用条件 |
|---|---|---|---|---|---|
| D27 | ~~EventLoop 分派循环不保护活动 Channel 存活：同轮 epoll 返回的 activeChannel 列表中，回调内析构 Channel 后，后续事件仍访问已释放内存（TSAN 已复现 heap-use-after-free）~~ **2026-10-22 已闭环**（TcpServer 完善轮 P1，用户裁决 tie/weak_ptr 方案：Channel 新增 tie(shared_ptr\<void\>) 存活锚点 + HandleEvent 分派前 lock 提升为守卫（宿主已亡则事件作废），TCPConnection::connectEstablished 登记锚点。保护语义：自身分派栈内销毁与分派轮外销毁（pending functor 引用在 activeChannel 循环后释放）；不保护「A 回调内清零 B 的全部引用」（muduo 同不支持）。用例改写为宿主形态启用（DestroyDuringDispatch 断言守卫生效）+ 新增 TieExpiredEventDropped；EchoTestHelper m_zombies 绕行删除改即时销毁，HalfClose 断言从绕行锁定翻转为正确语义（回显后 EOF/FIN 到达）；TDD 红面为 ASAN heap-use-after-free @ Channel.cc:60 ← EventLoop.cc:94 精确复现；常规 36/36 + ASAN/TSAN 绿 + repeat:10） | src/net/Channel.h/.cc（tie + HandleEvent 守卫）、src/net/TCPConnection.cc（connectEstablished） | test/net/ChannelTest.cc DestroyDuringDispatch（已启用）、TieExpiredEventDropped（新增） | test/integration/EchoTestHelper.h m_zombies（已删除） | 已完成 |

---

## 2. 本轮新发现问题（D28+）

> 测试重构实施过程中（新用例 FAIL、TSAN/ASAN 报告、压测异常）发现的问题在此登记。下一下可用编号：**D38**。
>
> **P11 全量 sanitizer 专项结果（2026-10-19，36 目标）**：TSAN 31/36 通过，5 个失败全部归因已知缺陷（D23×1、D33×4）；ASAN 33/36 通过，3 个失败归因（D23×1、D34×1、时序断言设计缺陷已修）。除登记项外未发现新的数据竞争或内存错误。

| 编号 | 发现阶段 | 发现场景 | 复现用例 | 证据 | 源码位置 | 建议修复 |
|---|---|---|---|---|---|---|
| D28 | P7 | TimestampTest.ToTimevalUsecTruncation 对现状源码 FAIL（tv_usec 得到 1735660800000000 而非 123456）。**2026-10-19 已闭环**（与 D24 一并修复：`tv_usec = m_us % 1e6`；正向用例已启用，现状锁定用例经裁决删除） | time/TimestampTest.TimevalUsecTruncation（已启用） | 断言失败输出：tv_usec = 1735660800000000 | src/time/Timestamp.cc:44-50 | 已完成 |
| D29 | P7 | SystemUtilTest.SnowflakeKeyConcurrentUnique 对现状源码 FAIL：8 线程 × 1000 次并发生成存在重复 key。**2026-10-19 已闭环**（用户裁决采纳：mutex 全程加锁 + `kSequenceMask=0xFFF` 十二位序列修正（原 0x1000 配 ~ 为清位语义错误）+ 序列耗尽等待下一毫秒 + 新毫秒序列归零；实施中修正过一处自研 bug——耗尽等待初版 `while(sequence==0)` 循环条件错误成死循环，改为 `do{...}while(currentMS==lastMS)`；用例启用、容忍用例删除；常规 5 单跑+10 连跑 + TSAN 双绿） | util/SystemUtilTest.SnowflakeKeyConcurrentUnique（已启用） | 三次实测唯一 key：7670/7556/7586（修复前） | src/util/Util.cc:82-107 | 已完成 |
| D30 | P9 | LogAppenderTest.RollFileOnSizeExceeded 对现状源码 FAIL：rollSize=1KB 写入 2.7KB 后目录仍只有 1 个文件。**2026-10-21 已闭环**（复核修正根因：主失效是 `RollFile()` 的 `if (now > m_lastRoll)` 把**同秒内的后续滚动请求整个跳过**（非仅文件名同名）；修复为应滚即滚（去掉跳过条件）+ 文件名拼入滚动计数 `m_rollCount`（同秒唯一）；正向用例启用、同构锁定用例删除；与 D21 联动后配置 "4KB" 语义完整恢复；36/36 绿） | log/LogAppenderTest.RollFileOnSizeExceeded（已启用） | 修复前：CountLogFiles=1 | src/log/LogAppender.cc:125-143 + LogAppender.h（m_rollCount 成员） | 已完成 |
| D31 | P9 | LogAppenderTest.AsyncDrainsAllLinesAfterStop 对现状源码 FAIL：异步 appender stop() 后 200 条日志仅 84 条落盘。**2026-10-21 已闭环**（threadFunc 退出前排空队列（drain 语义），与 D32 同轮修复；正向用例启用、宽松锁定用例删除；常规 5 连跑零丢行） | log/LogAppenderTest.AsyncDrainsAllLinesAfterStop（已启用） | 修复前：CountLines=84/200 | src/log/LogAppender.cc（threadFunc 尾部排空循环） | 已完成 |
| D32 | P9 | **AsynAppendFile 日志队列数据竞争**：生产者持锁 emplace，消费者无锁 front()/pop()。**2026-10-21 已闭环**（与 D31 同轮：消费段持锁取出、锁外落盘（避免 IO 阻塞生产者）；stop 改锁内判断置位/锁外 notify+join；~1/25 崩溃窗口消除——常规 5 连跑零崩溃；**TSAN LogAppenderTest 通过，TSAN known-failure 四目标全部归零**） | log/LogAppenderTest（AsyncStopDropsQueuedLogsLocksCurrent 复现载体已随修复删除） | TSAN deque 迭代器 data race 报告（修复前）；修复后 TSAN 干净 | src/log/LogAppender.cc:208-250 | 已完成 |
| D33 | P11 | **~Thread 的 detach 条件写反**（原 Q22 升级）。**2026-10-19 已闭环**（条件取反 `if (m_tid)`：仅运行中未 join 的线程 detach；TSAN 四目标全部解封——ThreadTest/ThreadPoolTest/LoggerManagerTest 过，LogAppenderTest 剩余失败归因转移为 D32） | 无独立用例（Thread 析构为所有用例的公共路径） | TSAN 复现栈：Thread.cc:54 → ConsumeThreadUserId CheckFailed | src/thread/Thread.cc:50-56 | 已完成 |
| D34 | P11 | Config/tomlc17 集成泄漏：解析失败的半成品内存（320B）无人释放。**2026-10-21 已闭环（用户升级 tomlc17 新版）**——新库在 `parse_keyvalue_expr` 的 parse_val 失败路径统一 `datum_free(&newval)` 递归清理未挂接半成品（正中旧版机制：bail 的 datum_free(&toptab) 只覆盖已挂接节点）；兼容性由用户在头文件给 union 加回 `data` 类型名解决（Config.cc 的 any_cast 用法保持）；NO_LEAK_CHECK 已移除，ASAN 泄漏检测常开 + TSAN + 常规 36/36 三绿 | ConfigTest 全体用例（泄漏检测已恢复常开，InvalidTomlThrows 为回归载体） | 修复前 LSan：Direct leak 320B（arr_emplace ← parse_inline_array ← toml_parse）；升级后零输出 | 3rd/toml/tomlc17.c（新版 parse_keyvalue_expr）；3rd/toml/tomlc17.h（union data 类型名兼容补丁，用户手动） | 已完成 |
| D35 | 修复轮 P8（D33） | TSAN ThreadPoolTest.RunAfterStopDropsTask 挂起超时（60s）——**D33 修复消除析构崩溃后暴露的被掩盖竞态**：BlockingQueue::over() 无锁写 m_isRunning + notifyAll，与 take()「持锁检查条件→原子进 wait」窗口交错时**通知丢失**，worker 永睡，stop() 的 join 永等（常规构建时序运气未触发）。**2026-10-19 已闭环**（用户裁决顺带修：无界版 :197 与 Bound 版 :123 两处 over() 各加 1 行 MutexLockGuard；ThreadPoolTest TSAN 解封，常规 36/36 绿） | TSAN 单跑该用例即挂（RUN 后无输出直到超时） | TSAN 挂起 + 代码级机制核实 | src/base/BlockingQueue.hpp:197-201；:123-128 | 已完成 |
| D36 | 审查轮（code-reviewer） | File::Write 部分写死循环：WriteUnlock 循环内恒写同一前缀（未推进 log+written）——磁盘满/ferror 场景死循环或数据重复。既有缺陷非修复轮引入；D30 滚动修复加大了流经此路径的写入量 | LogAppenderTest 未覆盖（需注入部分写错误才触发） | 审查者代码级核实（src/log/LogAppender.cc:65-84） | src/log/LogAppender.cc:65-84 | 修复：WriteUnlock 改 `WriteUnlock(log + written, remain)` 推进偏移并补失败退出条件；登记不修（用户裁决） |
| D37 | 审查修复轮终验 | TimerManager::cancel 在**调用线程**读 `timer->Expiration()` 打日志（Timer.cc:143），与 loop 线程 HandleRead:114 周期重插的 `setExpirationTime` 写无同步——TSAN 在 PeriodicTimerFiresMultipleTimes/PeriodicIntervalWithinTolerance 稳定报 data race（3/3 复现；修复轮 TSAN 专项时竞争窗口未命中属漏网）。**2026-10-22 已闭环**（日志读移入 RunInLoop 闭包——与写同线程串行；唯一跨线程读点，其余 Expiration 读写全在 loop 线程；TSAN 三连跑零报告 + TCPConnectionTest TSAN 同绿） | TSAN TimerTest 竞争报告（Timer.h:32 Expiration 读 vs Timer.h:64 写） | TSAN 栈确凿 | src/Timer.cc:140-152（已修） | 已完成 |
| D38 | TcpServer 完善轮 P4 | handleClose 在 `std::invoke(m_close_cb)` 返回**之后**还写 m_status、打日志——invoke 参数的临时 shared_ptr 若为最后引用，返回即触发 ~TCPConnection，随后两行访问已析构成员（UAF）。**2026-10-22 登记即闭环（结构加固）**：核实发现 P1 的 tie 守卫（被动路径）与 close() lambda 捕获（主动路径）已把析构隐性推迟到 handleClose 之后——公共 API 无红面可达（TDD 主动路径用例意外通过后核实）。仍修：`m_status = Disconnected` 前置到 close_cb 之前并删尾部访问——不变量从「依赖调用方 pin 的隐式约定」变为本地结构性保证（close_cb 可观察终态；回调后零成员访问）；CloseCallbackCanDropLastReference（被动 FIN 路径）锁定「守卫存活 + 终态前置」双重不变量 | TCPConnectionTest.CloseCallbackCanDropLastReference（被动路径形态） | 主动路径用例通过后代码级核实（lambda 捕获 pin 与 tie 守卫的双重遮蔽） | src/net/TCPConnection.cc handleClose（已修） | 已完成 |
| D39 | TcpServer 完善审查轮（code-reviewer C1） | **Stop 在回调内调用的同轮悬垂（两处）**：① stopInLoop 的 `m_listener.reset()` 在分派轮内销毁 Listener Channel（unique_ptr 宿主未 tie）——同轮 pending 的监听事件排后即 UAF；② 快照逐连接 `close()` 使其他连接的销毁链跑在当前连接的分派栈内（被关连接的 tie 守卫未激活）。**2026-10-22 登记即闭环（用户裁决方案 A′+补强）**：Listener 新增 Stop()（摘事件 + shutdown 拒新连，对象延迟销毁至 ~TcpServer）+ Channel::DisableAllEvent kNone 幂等快速路径；实施中 TDD 发现 ①不充分（ASAN 复跑仍 UAF，victim=连接 Channel）——**Stop 改 queue-only 调度**（公开 EventLoop::QueueInLoop，关闭链在分派轮外 doPendingFunctor 执行）；红面实证：修复前 ASAN heap-use-after-free @ Channel.cc:55 ← EventLoop.cc:94（审查者代码级推演的精确复现，一次命中）；回归锁定：TcpServerTest.StopFromMessageCallbackIsSafe（20 轮并发逼近）+ ListenerTest.StopRefusesNewConnections；ASAN 39/39 + 常规 39/39 + repeat:10 + TSAN 四目标绿 | TcpServerTest.StopFromMessageCallbackIsSafe、ListenerTest.StopRefusesNewConnections | ASAN UAF 栈（Channel.cc:55 ← EventLoop.cc:94；victim 分配栈=TCPConnection ctor 的 Channel） | src/net/Listener.h/.cc（Stop）、src/net/Channel.cc（kNone 快速路径）、src/net/EventLoop.h（QueueInLoop 公开）、src/net/TcpServer.cc（Stop 调度/stopInLoop/~TcpServer）（均已修） | 已完成 |
| D40 | TCP client 审查轮（code-reviewer #1） | **Connector::Stop 内联路径的定时器分派帧内自毁 UAF**：retry 模式下退避定时器触发 → TimerManager::HandleRead 帧 → startInLoop 遇致命 errno（EHOSTUNREACH/EACCES 等）→ failNoRetry 无视 m_retry 进用户失败回调 → 用户回调内 client.stop() → loop 线程内联 stopInLoop → m_timer_manager.reset() 在 TimerManager 自身分派帧内析构 → 返回路径 ResetTimer/回调尾部 m_HandlingExpiredTimer 访问已释放内存（TcpServer::Stop 因 C1 已 queue-only，Connector 重新引入同型）。**2026-10-23 登记即闭环**：Stop 统一 queue-only（QueueInLoop，m_connect=false 保持同步置位——与 TcpServer::Stop 对齐）；ConnectRefusedFiresFailed 加强为「回调内 Stop + fd>=0 断言」（审查 #1 可达形态 + INFO #9 移交语义锁定）；致命 errno 帧内精确复现需环境构造（EHOSTUNREACH），修复为结构性调度变更，以既有 retry/stop 用例回归（如实登记，不造脆弱用例）；常规 41/41 + ASAN 双目标绿 | ConnectorTest.ConnectRefusedFiresFailed（加强形态） | 审查者五帧调用栈静态推导 + 作者逐层核实 | src/net/TcpClient.cc Connector::Stop（已修） | 已完成 |
| D41 | UdpServer 审查轮（code-reviewer #2） | **ThreadPool::stop 丢弃已入队任务（排空缺失）**：threadFunc 的 while(m_isRunning) 在 stop 置位后直接退出循环——队列中已入队未执行任务全部丢弃。UdpServer「Stop 排空已提交任务」契约自 W1 起为**错误归因**（D35 修的是 over() 通知丢失，从未赋予排空语义）；原 StopDrainsQueuedTasks 用例的 500ms 裕度实为让队列自然消化，属假绿（「时序滑动」观察项的真实根因）。**2026-10-26 登记，用户裁决暂不改动 ThreadPool**：修复方案已呈并被用户细化为「threadFunc 原处理结构不动，m_isRunning=false 后再处理 m_taskQueue」（BlockingQueue over 后取尽语义成立；影响面已核查——RunAfterStopDropsTask 锁定入队侧丢弃、ThreadPool 真实调用方仅 UdpServer）；UdpServer Stop 文档如实化（不保证已入队执行 + 内核缓冲丢弃声明）；假绿用例翻转为 StopDropsQueuedTasksUntilDrainFixed（确定性锁定丢弃现状：在途完成==1、队列丢弃）；排空强断言（ThreadPoolTest 层）待修复时落地 | UdpTest.StopDropsQueuedTasksUntilDrainFixed（现状锁定） | 审查者组件级/组合级背离论证 + ThreadPool.cc 静态核实 | src/thread/ThreadPool.cc（已修） | 已完成 |
| — | **D41 处置记录**：2026-10-26 三轮交互后落地——用户否决调用线程执行快照方案（卡主线程），否决初版后细化为「threadFunc 原循环零改动 + 条件排空段（工作者线程内）」；take() 的 over 后空返回不阻塞语义经代码展示确认（等待谓词「运行中且空」）；最终形态：新增 `ThreadPool::stopAndDrain()`（m_drain_on_stop 标志 + over + join，stop() 丢弃语义零改动），UdpServer::Stop 接入；排障连带两项：①二分排障发现 run() 前置守卫为用户手动改动（非 D41 引入）——破坏「未启动内联」语义，用户裁决保留守卫+RunWithoutStartExecutesInline 翻转为 RunWithoutStartDropsTask；②二分残留 `false &&` 临时禁用补丁一度使 StopAndDrain 用例假失败（排障纪律教训登记）；ThreadPoolTest.StopAndDrainExecutesAllQueued（强断言：单 worker 全程保序 0..19）+ UdpTest 回翻弱形态；常规 41/41 + TSAN 双目标 5 连绿 |

---

## 3. 疑点清单（Q 系列）

> 尚未定性的可疑行为：可能是缺陷、可能是待确认的设计取舍。实施中逐一实测，确认缺陷后转正为 D 编号。

| # | 疑点 | 位置 | 状态 |
|---|---|---|---|
| Q3 | pending functor 抛异常 → 异常逃逸 Loop()，IO 线程死亡（doPendingFunctor 无 try/catch，已核实代码路径） | src/net/EventLoop.cc:150-153 | **已实测锁定**：EventLoopTest.FunctorExceptionEscapesLoopAndTerminates（死亡测试，abort 复现） |
| Q4 | Stop 后（m_looping=false）RunInLoop 投递的任务永不执行且无告警 | src/net/EventLoop.cc:114-141 | **已实测锁定**：EventLoopTest.TasksAfterStopAreDropped（任务确实被静默丢弃，无崩溃） |
| Q5 | ServerStatus 的 isAtomic 模板参数无效（条件类型定义被注释，恒为普通赋值） | src/net/ServerStatus.h:58-59 | 待实测 |
| Q6 | BlockingQueue::over() 后 put() 仍成功入队（不检查 m_isRunning） | src/base/BlockingQueue.hpp:247-260 | 待确认语义 |
| Q7 | Address 默认构造后 family()==0、getSockAddr() 返回零值 sockaddr，无 invalid 语义 | src/net/Address.h:18,44-49 | 待确认语义 |
| Q8 | CountDownLatch::coutDown 方法名拼写错误（应为 countDown），改名将破坏 API | src/thread/CountDownLatch.hpp:30-38 | 待用户裁决 |
| Q9 | ThreadPool worker 内任务抛异常是否杀死 worker 线程 | src/thread/ThreadPool.cc | 待实测 |
| Q10 | Timestamp 缺 operator==/!=（less_than_comparable 仅导出 >/\<= 等） | src/time/Timestamp.h:98-131 | 待用户裁决 |
| Q11 | Timestamp 负数 epoch 的 toDay/nextDay 因 C++ 整除向零取整可能错位 | src/time/Timestamp.h:52-63 | 待实测 |
| Q12 | Socket::isAvailable 用 m_fd>0（合法 fd 0 被判不可用）；close() 二次调用 assert | src/net/Socket.cc:141-150 | 待实测 |
| Q13 | EpollEventDispatcher 构造在 epoll_create1 失败时仅 LOG_ERROR 继续持有 -1 | src/net/EventDispatcher.cc:45-54 | 待确认设计 |
| Q14 | ~~Listener 构造期 bind 失败静默（不抛、Listen 返回 void 无法感知），端口被占用时客户端连接被拒绝且无上层告警~~ **2026-10-22 已闭环（审查轮 I1，用户裁决采纳）**：Listener 构造期 bind 失败抛 Exception::SocketError；TcpServer 构造期即建 Listener（同步上抛，消除幽灵启动）；listen 失败（bind 后近乎不可能）loop 线程内记 ERROR 且不启用事件；BindOccupiedPortIsSilent 翻转为 BindOccupiedPortThrows + TcpServerTest.PortInUseThrowsAtConstruction；ListenTwice 宽松语义维持锁定（残留注记） | src/net/Listener.cc（ctor/Listen）、src/net/TcpServer.cc（ctor，已修） | 已修复 |
| Q15 | ~~TCPConnection 双 close 非幂等：第二次 close 将状态改回 Disconnecting 并再次触发 CloseCallback（历史上还会留下 events=0 僵尸 epoll 注册——该副作用已随 D18 堵漏消失）~~ **2026-10-23 已闭环（审查轮 M1，用户裁决采纳）**：初始态改 Connecting（muduo 对齐，Disconnected 成为严格终态——旧实现初始/终态共用 Disconnected 无法区分「未建立可 close」与「已关闭应忽略」）；close()/shutdown() 入口终态守卫（幂等，回调恰一次）；connectEstablished 检查对齐 expect Connecting；析构断言放宽双态（Connecting=未建立无事件，析构安全）；DoubleCloseFiresCloseCallbackTwice 翻转为 DoubleCloseIsIdempotent（TDD 红面 close_calls==2→1）；常规 39/39 + TSAN 双目标绿（repeat:5） | src/net/TCPConnection.cc（已修） | 已修复 |
| Q16 | 异常默认文案存在复制粘贴痕迹：LoggerError 默认 what() 为 "ConfigError Error"、AddressError/SocketError 为 "XxxError Error" 语义重复 | src/exception/Exception.hpp:61-96 | **已实测锁定**：ExceptionTest.DefaultMessages |
| Q17 | Timestamp 默认构造不初始化 m_microSecondEpoch（=default 且无成员初始化器），读取默认构造对象的值不确定 | src/time/Timestamp.h:29 | 已核实代码；不可确定性地测试，文档记录 |
| Q18 | TimeZoneOffset/Date/Time 默认构造不初始化成员（=default），DateTime 两参构造的 offset() 输出不确定 | src/time/DateTime.h:17,61,98 | 已核实代码；不可确定性地测试，文档记录 |
| Q19 | ~~StrToFileSize 在字符串结尾判定 \*(str+1) 时越界读一字节（无单位输入路径）~~ **2026-10-19 已闭环**（随 D21 顺带修复：循环首 `'\0'` 先行判定） | src/util/Util.cc:17-19 | 已修复 |
| Q20 | 项目链接未开 -rdynamic，backtrace_symbols 输出仅地址/偏移、无函数名，Backtrace 的符号断言不可用 | CMakeLists（链接选项） | 已实测：SystemUtilTest.BacktraceFramesNonEmpty 放宽为非空断言；开 -rdynamic 后可加符号断言 |
| Q21 | ThreadPool stop() 后 run() 既不内联也不入队，任务被静默丢弃（m_threads 非空且 m_isRunning=false 分支为空） | src/thread/ThreadPool.cc:62-72 | **已实测锁定**：ThreadPoolTest.RunAfterStopDropsTask |
| Q22 | ~~~Thread 的 detach 条件疑似写反~~ **已升级为 D33**（TSAN 实锤崩溃） | src/thread/Thread.cc:50-56 | 见 D33 |
| Q23 | LoggerManager::LoadConfig(name) 的 name 参数被完全忽略，固定读取 sys_config 的 "logger" 键；且对已存在 logger 重复 LoadConfig 会追加 appender（不查重） | src/log/Logger.cc:110-113,148-160 | 已核实代码；LoadConfigAssemblesLoggers/LoadConfigUpdatesExistingLogger 覆盖主路径 |
| Q24 | Logger::addAppender 不查重：重复添加同一 appender 会使同一条日志被广播两次 | src/log/Logger.cc（addAppender 直接 push_back） | **已实测锁定**：LoggerTest.DuplicateAddBroadcastsTwice |
| Q25 | ~~ServerStatus::StatusToString 的 switch 无兜底 return~~ **2026-10-19 已闭环**（随 D5 顺带修复：switch 后补 `return "Unknown";`，用户裁决） | src/net/ServerStatus.h:28-49 | 已修复 |
| Q26 | ~~AsynAppendFile::Append(std::string_view) 以 data() 构造 std::string，遇内嵌 NUL 截断~~ **2026-10-21 已闭环**（随 D31+D32 顺带修复（用户裁决）：`emplace(data, size)` 按 size 截长） | src/log/LogAppender.cc | 已修复 |
| Q27 | Buffer::grow 滑动窗口分支要求 CheapPrependSize < m_read_idx，read_idx 恰为 8（CheapPrependSize）时断言失败 | src/Buffer.hpp:262-271 附近 | 已核实代码；待触发场景确认（2026-10-19 修复前复核发现） |
| Q28 | ~~Address 构造的 inet_pton 输入用 string_view::data()，无 NUL 终止保证（潜在越界读）~~ **2026-10-19 已闭环**（随 D20 顺带修复：函数体开头落 NUL 终止拷贝 `const std::string addrStr(strAddr)`，inet_pton 改读 c_str()） | src/net/Address.cc:26,43 | 已修复（用户裁决顺带） |
| Q29 | Config 的 asArray/asTable/asArrayTable 对 datum 元素 `std::move` 消费——datum 为 POD，move 即浅拷贝（源指针不清空）：产出的 Value4Toml 与原解析树**共享底层指针**，Handle 析构 toml_free 后访问即悬垂。旧库时代既有（非 tomlc17 升级引入）；现状测试均在 Handle 存活期内消费故未爆发。待裁决：深拷贝 or 生命周期文档化（Config::get 结果不得超出 Config 生命周期使用的契约声明） | src/Config.cc（asArray/asTable/asArrayTable 的 move 消费） | 2026-10-21 tomlc17 升级适配检查时发现；未修，待后续轮次 |
| Q30 | ~~TCPConnection 的 send/queueSendData 非 loop 线程读普通成员 m_status，与 loop 线程 handleClose 写构成理论数据竞争~~ **2026-10-22 已闭环**（TcpServer 完善轮 P4：m_status 改 std::atomic\<Status\>（默认 seq_cst）；TSAN 红面精确复现（close() 写 TCPConnection.cc:141 vs status() 读 TCPConnection.h:104）→ 修复后零报告；StatusReadableFromOtherThread 回归 + PeerClose 用例 RunInLoopSync 规避回收（测试线程直读）） | src/net/TCPConnection.h/.cc | 已修复 |
| Q31 | AsynAppendFile 自有 m_mutex 遮蔽基类 AppendFile::m_mutex：公有 Flush()（基类锁）被外部线程与消费者 AppendUnlock 并发调用时存在 m_file 竞态（常路径安全——文件状态仅消费者触碰；现有测试未覆盖并发 Flush）。建议移除遮蔽成员或统一锁域 | src/log/LogAppender.h:154 + LogAppender.cc Flush | 2026-10-22 审查轮发现；未修 |
| Q32 | Socket 构造不校验 detail::CreateSocket 失败：fd=-1 照常构造——后续 bind 在 Debug 下死于 assert(m_fd>0)、Release 下抛误导性 "bind failed"（实为创建失败）。UdpServer 已局部兜底（M9：构造期 isAvailable 检查抛 SocketError）；层修复（Socket 构造抛 SocketError）需评估既有调用方与用例，待后续轮 | src/net/Socket.cc（Socket(Family,Type) 构造） | 2026-10-26 UdpServer 审查轮发现；未修 |

---

## 4. 空缺编号说明

- **D12、D25、D26**：CLAUDE.md 提及「已知缺陷 D1–D26」，但现有测试代码与（已删除的）docs 中均无对这三个编号的任何引用，语义不可考。**永久空缺，编号不复用**。
- **O3**：历史遗留的 O 系列编号（Observation），仅此一条，此后新问题统一用 D 系列。

---

## 5. 移除候选（死代码，不写测试）

| 文件 | 理由 | 处置建议 |
|---|---|---|
| src/base/Observer.hpp | 遗留 namespace uf，src 内零引用 | 建议随下次清理任务移除 |
| src/base/callback.h | 遗留 namespace uf，9 行，src 内零引用 | 建议随下次清理任务移除 |
