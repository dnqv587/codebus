# 已登记缺陷逐项修复设计与计划（简单优先 · 交互式）

> 状态：实施中（P0 完成，自 P1 起逐项交互推进）
> 关联：[known-issues.md](../known-issues.md)（缺陷登记）、[2026-10-19-unit-test-refactor-design.md](2026-10-19-unit-test-refactor-design.md)（发现这些缺陷的测试重构）

## 1. 需求背景

单元测试体系重构（36 目标全绿）过程中，通过单元/集成用例、TSAN/ASAN 专项在源码中登记了 **30 项活跃缺陷**（D1–D11、D13–D21、D23、D24、D27–D34 + O3）。测试阶段遵循「只登记不修源码」原则，修复另立任务——即本任务。

**用户要求**：

1. 分析问题列表，按优先级排序，**先处理相对简单、改动较小的问题**
2. **单次只处理一个问题**，交互式推进
3. 交互时**解决方案与具体待修改代码均需用户确认**，确认完成后再进行改动；完成后再处理下一个

## 2. 优先级分析方法

修复前对全部 30 项做了源码级复核（逐项确认缺陷存在、精确定位、最小修复草案、波及面）。结论：

- **30 项全部属实**，仅 3 处登记偏差（已在 known-issues.md 修正）：
  - D34：泄漏点在 tomlc17 库内（`parse_inline_array` 的分配未被其 free 路径覆盖），`Config::Handle` 析构已正确调用 `toml_free`
  - D17：IPv6 重载（Address.cc:51-60）与已登记的 IPv4 重载同病，修复须两处一起
  - D24/D28：登记中 `m_us` 为笔误，实际成员名 `m_microSecondEpoch`
- 另发现 4 处未登记相邻隐患 → Q25–Q28（见 known-issues.md 疑点清单），本轮仅登记不动手
- 排序依据：**源码改动行数 → 测试三动作工作量 → 绕行回收量 → 验证成本 → 风险**，简单者先行；同根因/同函数族合并为一个处理单元（D24+D28、D21+Q19、D31+D32）

## 3. 优先级总表

### T1 单点微修（源码 ≤3 行）

| # | 编号 | 修复草案（最小） | 源码位置 | 测试动作 |
|---|---|---|---|---|
| 1 | D24+D28 | `tv_nsec=(m_us%1e6)*1000`；`tv_usec=m_us%1e6` | src/time/Timestamp.cc:40,48 | 启用 2 个 DISABLED；处置 1 个锁定用例 |
| 2 | D3 | Reserve 扩容后补 `m_capacity` 同步 | src/Buffer.hpp:206 | 启用 DISABLED_ReserveUpdatesCapacity |
| 3 | O3 | toString 拼接 `":"` | src/net/Address.cc:137 | 启用 DISABLED_ToStringHasColon |
| 4 | D20 | inet_pton 判定 `<0` → `!= 1` | src/net/Address.cc:26,43 | 启用正向 + 删锁定用例 |
| 5 | D4 | TimeWait `==` → `!=` | src/thread/Condition.hpp:36 | 启用 DISABLED + 翻转 2 个锁定用例 |
| 6 | D5 | setStatus 补 return（返回旧状态/改 void 到达时裁决） | src/net/ServerStatus.h:53-55 | 3 个 SIGILL 用例随启 |
| 7 | D23 | take `T&&`→`T`（Bound 版 :163 同修） | src/base/BlockingQueue.hpp:241 | ASAN 红面自动转绿 + 启用 ThreadPool 2 个 DISABLED |
| 8 | D33 | ~Thread 条件取反 `if (m_tid)` | src/thread/Thread.cc:51 | 无独立用例；TSAN 四目标解封验证 |
| 9 | D14 | 析构 `::close(m_EpollFd)` | src/net/EventDispatcher.h:59 | 启用 DISABLED_DestructorClosesEpollFd |
| 10 | D13 | WritableSize 断言 `>` → `>=` | src/Buffer.hpp:158 | 启用 DISABLED_ZeroCapacity + 回收 3 处绕行 |
| 11 | D17 | 两重载 `ip.resize(strlen(ip.c_str()))` | src/net/Address.cc:51-71 | 启用正向 + 删 6 处 c_str() 绕行 |

### T2 函数级小修（3–15 行）

| # | 编号 | 修复草案 | 源码位置 |
|---|---|---|---|
| 12 | D2 | Slice::operator<=> 手写 constexpr 逐字节+长度比较 | src/Buffer.hpp:99-101 |
| 13 | D21(+Q19) | 补 `case 'K'` + 前置 `'\0'` 检查（与 D30 联动：配置 "4KB" 将从 4 字节变 4096） | src/util/Util.cc:15-42 |
| 14 | D15 | getSocketErrno 改 `getsockopt(SO_ERROR)`（消除误清 SO_REUSEADDR 副作用） | src/net/Socket.cc:185-193 |
| 15 | D16 | FD_CLOEXEC 经 F_SETFD 设置 | src/net/Socket.cc:195-206 |
| 16 | D29 | generateSnowflakeKey 加锁 + SEQUENCE_MASK 语义修正 | src/util/Util.cc:76-89 |
| 17 | D1 | 构造条件 `<=0` 抛；drain `std::move`→`std::swap` | src/base/BlockingQueue.hpp:98,116 |
| 18 | D6 | ~TimerManager：DisableAllEvent + RemoveChannel + close | src/Timer.cc:182-183 |
| 19 | D18 | ~Listener 二次 RemoveChannel（方案 A/B 到达时裁决） | src/net/Listener.cc:25-28 |
| 20 | D7/D19 | refresh 改 `ClockType::now()` + 同步 erase 重插 | src/Timer.cc:197-210 |
| 21 | D30 | RollFile 文件名拼滚动计数（根因：秒级文件名同秒同名重开） | src/log/LogAppender.cc:127-145 |
| 22 | D31+D32 | 消费段持锁取出；stop 锁内置位/锁外 join/排空 | src/log/LogAppender.cc:208-254 |

### T3 功能补全与库侧

| # | 编号 | 内容 | 源码位置 |
|---|---|---|---|
| 23 | D8 | asArrayTable 激活注释（组合 asArray+asTable） | src/Config.cc:448-462 |
| 24 | D10 | send(Buffer*) 实现 | src/net/TCPConnection.cc:150-152 |
| 25 | D34 | tomlc17 库侧泄漏（改 3rd/ 前需用户确认） | 3rd/toml/tomlc17.c |
| 26 | D11 | connectEstablished 补全（较大功能） | src/net/TCPConnection.h/.cc |
| 27 | D9 | send 按值捕获 Slice（排 D11 后方可验证） | src/net/TCPConnection.cc:141-148 |

### T4 架构级

| # | 编号 | 内容 |
|---|---|---|
| 28 | D27 | EventLoop 分派循环的 Channel 存活保护（tie/weak_ptr 或延迟销毁协议），单独设计轮 |

## 4. 交互式处理协议（每个处理单元一轮）

1. **问题卡**：缺陷语义 / 源码现状（带行号）/ 证据 / 波及面 / 修复方案（含备选与决策点）
2. **方案确认**：AskUserQuestion 裁决方案选择/语义/是否顺带修相邻项
3. **代码确认**：全部待改动代码（源码 + 测试动作 + 绕行回收）以 before/after 完整展示，**用户逐项确认后才动工**
4. **实施**：按确认清单改动（先测试红 → 源码绿 → 删绕行）
5. **验证**：`ctest -R <目标>`，按性质追加 TSAN/ASAN，输出证据
6. **登记与摘要**：known-issues.md 标记闭环；核对落地与确认内容一致；用户确认后进入下一个

## 5. 验证策略

| 时机 | 动作 |
|---|---|
| 每问题 | `ctest -R <受影响目标>`（build/ 常规） |
| 内存类（D13/D23/D34） | 追加 ASAN 对应目标 |
| 并发类（D23/D33/D29/D31+D32） | 追加 TSAN 对应目标；D23/D33 后核对 TSAN known-failure 四目标解封 |
| 每梯队完成 | 全量 ctest（36 目标） |
| 终局 | 36×{常规,TSAN,ASAN} 矩阵 + 核心目标 `--repeat until-fail:10` + TODO(Dxx) grep 终态核对 |

## 6. 风险与对策

| 风险 | 对策 |
|---|---|
| D21 修复改变日志滚动行为（联动 D30） | 到达时明示，两者都完成后核对滚动场景 |
| D18 方案选择影响 D6 析构模式 | D6 在前，采用与已定方案一致的摘除方式 |
| EchoTestHelper 绕行回收（D13/D18）触及集成稳定性 | NetworkStressTest 全目标 + repeat 验证；D27 的 m_zombies 绕行不动 |
| D34 涉及第三方库 | 调查结论先行呈现，改 3rd/ 前必须用户确认 |
| 修复引入回归 | TDD 先红后绿；每梯队全量 ctest；终局 sanitizer 矩阵 |

## 7. 进度记录

| 处理单元 | 状态 | 备注 |
|---|---|---|
| P0 准备（本文档 + 登记修正 + Q25–Q28） | 完成 | 2026-10-19 |
| P1 D24+D28 | **完成** | 2026-10-19：两行取余修复；启用 2 用例、删 1 锁定用例（用户裁决）；TimestampTest/TimerTest 绿 |
| P2 D3 | **完成** | 2026-10-19：1 行容量同步；用例启用；Buffer/Slice/NetworkStress 绿 |
| P3 D17+O3 | **完成** | 2026-10-19：O3 依赖 D17 提前合并；两重载截断 + 冒号拼接；7 处绕行回收；5 目标绿 |
| P4 D20(+Q28) | **完成** | 2026-10-19：两处 `!= 1` + NUL 终止拷贝（Q28 顺带，用户裁决）；启用 InvalidIpThrows、删锁定用例；4 目标绿 |
| P5 D4 | **完成** | 2026-10-19：返回值翻转；启用 2 用例、删 1 重复锁定用例；thread 模块 7 目标绿 |
| P6 D5(+Q25) | **完成** | 2026-10-19：方案 A 返回旧状态（用户裁决）+ StatusToString 兜底（Q25 顺带，用户裁决）；3 用例启用；5 目标绿 |
| P7 D23 | **完成** | 2026-10-19：两处 T&&→T；3 用例启用；常规+ASAN 双绿（build-asan 重建）；TSAN 留待 D33 后四目标统一核对 |
| P8 D33(+D35) | **完成** | 2026-10-19：detach 条件取反；TSAN 四目标解封；暴露并顺带修复 D35（over() 丢失唤醒，用户裁决）；LogAppenderTest 剩余归因 D32（T2 计划内）；常规 36/36 |
| P9 D14 | **完成** | 2026-10-19：析构关 fd；用例启用（补 m_loop.reset() 适配 fixture）+ MultiLoopClientStorm 恢复 3 loops（用户裁决）；36/36 绿 |
| P10 D13 | **完成** | 2026-10-19：断言 >=；BufferTest 侧全回收（随机模型恢复恰好写满覆盖）；EchoTestHelper 保留实现更新归因（用户裁决）；36/36 + ASAN 绿。**T1 梯队（11 处理单元）全部完成** |
| P11 D2 | **完成** | 2026-10-19：用户裁决「无消费者即删除」——全库核查零比较使用后删除 operator<=> 与 DISABLED_CompareByContent；全量构建零编译错 + 3 目标绿 |
| P12 D21(+Q19) | **完成** | 2026-10-19：补 KB 分支 + 结尾先行判定；用例启用；Logger 联动复核绿 |
| P13 D15 | **完成** | 2026-10-19：getsockopt(SO_ERROR)；用例启用 10 连跑稳 |
| P14 D16 | **完成** | 2026-10-19：F_SETFD 设置 FD_CLOEXEC；用例启用；SocketTest 绿 |
| P15 D29 | **完成** | 2026-10-19：mutex+0xFFF+耗尽等待（初版自研死循环 bug 已当场修正并向用户说明）；TSAN 绿；附带发现 lib/ 共享输出踩坑并补 CLAUDE.md；36/36 绿 |
| P16 D1 | **完成** | 2026-10-19：条件取反 + std::swap + 断言对象修正（第三处笔误，实施中发现）；用例翻转+启用；36/36 绿 |
| P17 D6 | **完成** | 2026-10-19：析构补实现；ManagerGuard 回收 10 处泄漏；NO_LEAK_CHECK 移除 ASAN 绿；36/36 绿 |
| P18 D18 | **完成** | 2026-10-20：方案 A（用户裁决，含两分支路径详述确认）；6+1 处绕行回收；NO_LEAK_CHECK 移除 ASAN 绿；36/36 绿 |
| P19 D7/D19 | **完成** | 2026-10-21：steady 时钟 + 同步 erase 重排 + ResetTimer；实施中修正 ClockType 作用域（TimerManager 类内别名在 Timer 不可见，改全限定）；36/36 绿 |
| P20 D30 | **完成** | 2026-10-21：去同秒跳过 + 文件名序号；根因修正登记（主失效为跳过条件）；36/36 绿 |
| P21 D31+D32(+Q26) | **完成** | 2026-10-21：消费段持锁+退出排空+锁外 join+Q26 截长（用户裁决）；TSAN known-failure 四目标归零；36/36 绿。**T2 梯队（P11–P21）全部完成** |
| P22 D8 | **跳过（用户裁决：功能延后）** | 保持登记不修，用例维持 DISABLED |
| P23 D11 | **完成** | 2026-10-21：用户裁决 D11 提前（原排 T3 后段）；connectEstablished 补全 + 完整回显用例；解锁 D9/D10 验证路径；36/36 绿 |
| P24 D9+D10 | **完成** | 2026-10-21：queueSendData 拷贝语义 + Buffer 同步/异步分支；两用例改写可断言；ASAN 绿；36/36 绿 |
| P25 D34 | **完成** | 2026-10-21：用户升级 tomlc17 新版（新库已修半成品清理）+ 头文件 union 类型名兼容补丁（用户手动）；ASAN 泄漏检测常开/TSAN/常规三绿；顺带发现 Q29（POD 浅拷贝悬垂，旧库既有）登记。**T3 梯队收官**（D8 用户裁决延后） |
| P26 审查修复轮 | **完成** | 2026-10-22：code-reviewer 对抗性审查（GO-WITH-CHANGES）后逐条处置——C1（handleClose 双态断言+被动 FIN 回归用例，TDD 红→绿）、I1（D13 残留绕行回收+注释更正）、I2（refresh 用例加强证伪点）、I3（Q15 机制描述更正）、I4（空转用例补强断言）、M1/M2/M4（注释更正）、M5/M3/M6 登记 D36/Q30/Q31；终验新发现 D37（Timer cancel 日志跨线程读，TSAN 3/3 稳定红）登记并修复，PeerClose 用例的 status 断言移入 loop 线程规避 Q30 窗口；常规 36/36 + TSAN 全绿 |
| 剩余 | — | T4：D27（架构级单独设计轮）；D8 延后（用户裁决）；Q29/Q30/Q31/D36 登记 |
