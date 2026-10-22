# 单元测试体系重构设计文档

| 项 | 内容 |
|---|---|
| name | 单元测试体系重构（一单元一目标，网络核心） |
| brief | 将 test/ 下 15 个混合测试文件重组为镜像 src/ 结构的 36 个单元测试目标，按六维度梳理全量测试场景，迁移并完整保留缺陷登记（D 编号体系），网络模块增加高并发与内存安全（TSAN/ASAN）排查 |
| attention | 本文档是实施的权威依据；配套的缺陷登记活文档为 [docs/known-issues.md](../known-issues.md)，两文档的 D 编号保持一致；实施分阶段交付，每阶段 ctest 全绿（网络阶段追加 sanitizer 门槛）才进入下一阶段 |

---

## 0. 背景与目标

### 0.1 现状问题

test/ 下 15 个测试文件（约 3951 行、153 用例：131 启用 / 22 DISABLED）按"模块级文件"组织：

1. **单文件混测多个被测类**：ThreadTest.cc 混测 7 个类、LogTest.cc 混测 3 个子模块、UtilTest.cc 混测 4 组工具（ProcessInfo 实际在 src/thread/）
2. **套件粒度不一**：BufferTest 拆 2 套件、TimeTest 拆 5 套件，ConfigTest/LogTest 单套件大杂烩；ServerStatus 搭车在 EventLoopTest
3. **覆盖缺口**：EventDispatcher 零直接覆盖；大量单元缺边界/错误/并发场景
4. **helper 职责混杂**：NetworkTestHelper.h 同时含单元设施（LoopThread/RunInLoopSync）与集成载体（EchoServer/EchoClient），内含 3 处绕源码缺陷的临时处理
5. **缺陷登记散落**：D1–D24（除 D12）+ O3 以 DISABLED 用例 + TODO 注释散布，无集中对照表；EchoServer 僵尸连接 UAF（TSAN 已复现）完全未登记

### 0.2 目标

1. 一单元一测试目标（35 单元 + 1 集成 = **36 目标**），目录镜像 src/ 结构
2. 每单元按六维度（正常功能/边界条件/错误处理/并发安全/生命周期 RAII·fd/压力）梳理场景，系统性补齐 gap
3. **网络模块为核心**：全面覆盖网络库场景 + 高并发下的线程安全与内存安全（越界/UAF/泄漏）排查，每网络阶段以 TSAN+ASAN 通过为门槛
4. 缺陷登记全量迁移（编号不变），集中登记于 [docs/known-issues.md](../known-issues.md)
5. 集成/压测独立归类 test/integration/
6. 空壳模块（TcpServer/EventLoopThreadPool/Hook/UDP）只预留场景清单

### 0.3 需求决策记录

| 决策点 | 结论 | 确认方式 |
|---|---|---|
| 组织粒度 | 一单元一测试目标，目录镜像 src（根级源文件的测试放 test 根） | 用户确认 |
| 空壳模块 | 不写测试，预留场景清单 | 用户确认 |
| 集成测试 | 独立 test/integration/ | 用户确认 |
| 缺陷登记 | 全量迁移 + 独立问题文档 docs/known-issues.md | 用户确认 |
| 模块深度 | 全模块同等深度，网络模块额外高并发与 sanitizer 排查 | 用户确认 |

---

## 1. 单元划分

### 1.1 关键裁决（均经源码核实）

| # | 裁决点 | 结论 | 理由 |
|---|---|---|---|
| 1 | TypeTest/MacroTest 独立 | 保留，定位"契约测试" | 镜像独立头文件；Macro.h 的 ASSERT 经 assert_fail 走 abort()（src/base/Macro.h:27-35），需 EXPECT_DEATH 死亡测试，独立目标便于 sanitizer 下跳过；TypeTest 兼承 copyable/noncopyable/less_than_comparable 三个基类头的编译期契约（static_assert），它们不值得各自成目标 |
| 2 | ASSERT_THROW/CHECK_NOTNULL/LOG_THROW 归属 | 归 log/LoggerTest | 三者定义在 src/log/Logger.h:40-48（依赖 Logging 记日志后抛出），不在 Macro.h；MacroTest 只测 Macro.h 内容 |
| 3 | DateTime 四类聚合 | 保留聚合，目标内 4 套件 | Date/Time/TimeZoneOffset/DateTime 同在 src/time/DateTime.h，DateTime 直接聚合前三个为成员，拆开必然大量重复构造 |
| 4 | Timer+TimerManager | 不拆，一目标两套件 | Timer 构造函数私有（src/Timer.h:55），只能由 TimerManager 创建；Timer::cancel/refresh 回调 manager |
| 5 | ProcessInfoTest | 独立（虽仅 2 函数） | 镜像 src/thread/ProcessInfo.h 保持导航一致；并入 ThreadTest 会复刻"一文件混多类"坏味道；注册成本一行 |
| 6 | Thread+CurrentThread | 一个单元 | src/thread/Thread.h:11 `class Thread : CurrentThread` 私有继承，二者为整体 |
| 7 | log 拆 5 目标 | 补 LoggingTest | src/log/Logging.h（Logging 流式 RAII + LogContext + LogLevel）是所有 LOG_* 宏的载体，"析构才提交"契约值得独立场景 |
| 8 | D14 用例随缺陷迁移 | 迁至 net/EventDispatcherTest | 缺陷实体是 ~EpollEventDispatcher =default 不关闭 m_EpollFd（src/net/EventDispatcher.h:59）；用例改名 DISABLED_DestructorClosesEpollFd，原名记入对照表 |
| 9 | D1 预留方式 | 升级为"现状锁定用例"+DISABLED 正向场景 | D1 现状：传入正容量构造即抛（src/base/BlockingQueue.hpp:97-102 条件写反）；启用的 EXPECT_ANY_THROW 用例锁住当前行为，源码被无意改动时测试立即报警，修复后翻转为正向断言 |
| 10 | Channel 跨线程操作 | 不写"故意 UB"用例 | Channel 设计上必须在 loop 线程操作，跨线程操作属 UB 面，以文档化+疑点登记呈现 |
| 11 | Observer.hpp/callback.h | 不写测试，登记移除候选 | 遗留 namespace uf，src 内零引用，死代码 |
| 12 | MacroReflaction.hpp | 不独立成单元 | 被 src/log/LogFormatter.cc 使用（批量生成 FormatItem），经 LogFormatterTest 全量间接覆盖 |

### 1.2 单元划分总表（35 单元 + 1 集成 = 36 目标）

目标名 = 文件名去 .cc，全局唯一（保持 `ctest -R BufferTest` 既有用法兼容）。

#### test/ 根（4，镜像 src 根级文件）

| 目标 | 被测源文件 | 单元职责 | 用例来源 |
|---|---|---|---|
| SliceTest | src/Buffer.hpp（Slice） | 字节视图的构造、迭代、转换、比较契约 | 迁移 SliceTest 套件 11 用例（含 D2）+ 新增 |
| BufferTest | src/Buffer.hpp（Buffer） | 三区缓冲读写、增长、压实、模型对照 | 迁移 BufferTest 套件 26 用例（含 D3、D13）+ 新增 |
| ConfigTest | src/Config.h/.cc | TOML 加载、类型访问、路径查询、生命周期 | 迁移 13 用例（含 D8）+ 新增 4 |
| TimerTest | src/Timer.h/.cc | timerfd 定时器添加/触发/取消/周期（Timer+TimerManager 两套件） | 迁移 8 用例（含 D6、D7/D19）+ 新增 4 |

#### test/base/（5）

| 目标 | 被测源文件 | 单元职责 | 用例来源 |
|---|---|---|---|
| TypeTest | type.h、copyable.hpp、noncopyable.hpp、less_than_comparable.hpp | 类型别名、implicit_cast、is_one_of、字面量、基类契约 | 新增 |
| MacroTest | Macro.h | LIKELY/CONSTEVAL/CASE_STRING/ASSERT 宏行为 | 新增 |
| ViewTest | view.h | reference_wrapper 视图构造、Peer/NativeHandle、拷贝移动 | 新增 |
| SingletonTest | Singleton.hpp | Meyers 单例唯一性与并发首次初始化 | 新增 |
| BlockingQueueTest | BlockingQueue.hpp | 无界/有界阻塞队列生产消费同步语义 | 迁移 5 用例（含 D23）+ D1 锁定 + 新增 3 |

#### test/exception/（1）

| 目标 | 被测源文件 | 单元职责 | 用例来源 |
|---|---|---|---|
| ExceptionTest | Exception.hpp | 异常类型层次与 what() 文案契约 | 新增 |

#### test/log/（5，自底向上依赖序）

| 目标 | 被测源文件 | 单元职责 | 用例来源 |
|---|---|---|---|
| LogFormatterTest | LogFormatter.h/.cc（间接覆盖 MacroReflaction） | 占位符解析、Format 输出、suffix 穿插 | 迁移 2 + 新增 4 |
| LogAppenderTest | LogAppender.h/.cc | 同步/异步文件落地、滚动、Flush、Stdout | 迁移 2 + 新增 5 |
| LoggingTest | Logging.h | 流式日志对象 RAII 提交、LogContext 字段、LogLevel | 拆自 LogTest + 新增 3 |
| LoggerTest | Logger.h（Logger 类 + LOG_\* 宏 + ASSERT_THROW/CHECK_NOTNULL/LOG_THROW） | 级别过滤、appender 增删广播、断言宏 | 迁移 5（含 D22）+ 新增 5 |
| LoggerManagerTest | Logger.h（LoggerManager） | getLogger 创建/复用、LoadConfig 装配 | 迁移 1 + 新增 3 |

#### test/net/（8）

| 目标 | 被测源文件 | 单元职责 | 用例来源 |
|---|---|---|---|
| AddressTest | Address.h/.cc | IPv4/IPv6 地址构造、转换、字符串化、sockaddr_cast | 迁移 8（含 D17/D20/O3）+ 新增 4 |
| SocketTest | Socket.h/.cc | fd RAII、IO 读写、连接建立、套接字选项 | 迁移 8（含 D15/D16）+ 新增 8 |
| ChannelTest | Channel.h/.cc | fd 事件注册与回调分派 | 迁移 6 + 新增 4（含 D27 新增 DISABLED） |
| EventDispatcherTest | EventDispatcher.h/.cc | epoll 封装的事件注册/分发/移除（**新增直接覆盖**） | D14 迁入 + 新增 8 |
| EventLoopTest | EventLoop.h/.cc | 事件循环、跨线程任务、唤醒、生命周期 | 迁移 7 + 新增 5 |
| ServerStatusTest | ServerStatus.h | 状态机流转与字符串化（自 EventLoopTest 独立） | 迁移 2（含 D5）+ 新增 2 |
| ListenerTest | Listener.h/.cc | accept 监听器回调与 fd 管理 | 迁移 4（含 D18）+ 新增 2 |
| TCPConnectionTest | TCPConnection.h/.cc | TCP 连接状态机、发送路径、回调 | 迁移 7（含 D9/D10/D11）+ 预留（多被 D11 阻塞） |

#### test/thread/（7）

| 目标 | 被测源文件 | 单元职责 | 用例来源 |
|---|---|---|---|
| MutexTest | Mutex.hpp | 互斥锁/读写锁及 RAII guard 的互斥与并发 | 迁移 2（自 ThreadTest）+ 新增 3 |
| ConditionTest | Condition.hpp | 条件变量等待/通知/超时 | 迁移 1（含 D4）+ 新增 2 |
| SemaphoreTest | Semaphore.hpp | 信号量计数同步 | 迁移 1 + 新增 2 |
| CountDownLatchTest | CountDownLatch.hpp | 倒计时门闩 | 迁移 1 + 新增 2 |
| ThreadTest | Thread.h/.cc + CurrentThread.h/.cc | 线程启动/join/命名/绑核 + 线程身份 | 迁移 2 + 新增 5 |
| ThreadPoolTest | ThreadPool.h/.cc | 线程池生命周期与任务分发 | 迁移 2 + 新增 4 |
| ProcessInfoTest | ProcessInfo.h/.cc | 进程标识（Pid/hostname）契约 | 迁移 1（自 UtilTest）+ 新增 1 |

#### test/time/（2）

| 目标 | 被测源文件 | 单元职责 | 用例来源 |
|---|---|---|---|
| TimestampTest | Timestamp.h/.cc | 微秒时间戳运算、格式化、POSIX 转换 | 迁移 6（含 D24）+ 新增 5 |
| DateTimeTest | DateTime.h/.cc | Date/Time/TZOffset/DateTime 校验与格式化 | 迁移 6 + 新增 5 |

#### test/util/（3）

| 目标 | 被测源文件 | 单元职责 | 用例来源 |
|---|---|---|---|
| TimeUtilTest | Util.h（TimeUtil） | 运行时间与自定义 epoch | 迁移 2 + 新增 |
| StringUtilTest | Util.h（StringUtil） | BaseFileName、StrToFileSize | 迁移 2（含 D21）+ 新增 1 |
| SystemUtilTest | Util.h（SystemUtil） | Backtrace、核心数、雪花 key | 迁移 2 + 新增 3 |

#### test/integration/（1）

| 目标 | 被测 | 职责 | 用例来源 |
|---|---|---|---|
| NetworkStressTest | 多组件协同（Listener+Channel+Socket+EventLoop+EchoServer） | echo 服务并发/大包/风暴/半关闭/全双工 | 迁移 7 + 新增 5（高并发专项） |

---

## 2. 场景矩阵与 Gap 分析

标注约定：`[正]`正常功能 `[界]`边界条件 `[错]`错误处理 `[并]`并发安全 `[命]`生命周期(RAII·fd) `[压]`压力。来源：`迁移:<旧用例>` / `新增` / `新增·Dxx 阻塞` / `现状锁定·Dxx`。

### 2.1 test/ 根

#### SliceTest（迁移 11 + 新增）

- 迁移：5 种构造（迭代器/char\*/void\*/string_view/默认）、迭代与索引、ToString 保留内嵌 NUL、流输出、浅拷贝语义、DISABLED_CompareByContent（D2）
- 新增：
  - `[界]` 空 Slice 与零长度 string_view 构造的 `empty()/data()` 契约
  - `[错]` `operator[]` 越界触发 assert——死亡测试，sanitizer 下 GTEST_SKIP
  - `[界]` 空指针 void\* 构造存活（noexcept 契约）

#### BufferTest（迁移 26 + 新增）

- 迁移：构造不变量、写/读/peek/skip/truncate/findEol、prepend、增长触发、1MB 大写入、滑动窗口压实、拷贝语义、随机模型 500 步、DISABLED_ZeroCapacity（D13）、DISABLED_ReserveUpdatesCapacity（D3）；D13 另有两处"避开恰好写满"绕行（保留并标 TODO(D13)）
- 新增：
  - `[界]` Read 请求长度大于 ReadableSize 时退化为 ReadAll（src/Buffer.hpp:229-236 else 分支，现状无显式用例）
  - `[界]` Prepend 超过 PrependableSize 触发 assert（死亡测试或文档化）
  - `[压]` 随机模型扩展：500→2000 步、种子化可复现（失败输出种子）
  - `[命]`（D13 修复后）`Buffer(0)` 构造、WriteExactCapacity 无绕行、随机模型移除避开约束

#### ConfigTest（迁移 13 + 新增 4）

- 迁移：LoadString 全标量/日期/数组/表、嵌套路径、缺 key 返回 NullValue、类型不匹配 nullopt、未 Load 先 get 抛、非法 TOML 抛、LoadFile、Reload、ValueOutlivesConfig、DISABLED_ArrayTable（D8）
- 新增：
  - `[并]` 8 线程并发 get/seek 同一与不同 key，值一致（Config.h 的 shared_mutex 零覆盖）
  - `[错]` LoadFile 不存在的文件抛 ConfigError（现状只测非法 TOML 字符串）
  - `[界]` Load 空字符串/仅注释，get 任意 key 返回 NullValue
  - `[正]` SYS_CONF 宏 + sys_config 全局对象冒烟

#### TimerTest（迁移 8 + 新增 4）

- 迁移：一次性/周期/取消/属性/跨线程添加/8×100 并发添加取消、DISABLED_ManagerDestructorClosesTimerFd（D6）、DISABLED_RefreshReArmsTimer（D7+D19）；fixture 的 CreateManager() 有意泄漏保留并标 TODO(D6)
- 新增：
  - `[正]` 已过期 when（now-1s）添加后立即触发
  - `[正]` 周期定时器触发次数与间隔容差（±50ms）
  - `[界]` 一次性触发后等待 2 周期确认不再触发
  - `[正]` 回调执行期间 addTimer 新定时器（TimerManager 哨兵 m_sentry 处理分支，零覆盖）
  - `[并]` 并发规模加深 8×100 → 16 线程
  - `[命]`（D6 修复后）TimerManager 栈对象构造析构 timerfd 关闭的 FdCount 断言

### 2.2 test/base/

#### TypeTest（全新）

- `[正]` 字面量值断言：`1_KB==1024`、`1_MB==1024*1024`、`1_GB`/`1_TB`（off64_t）
- `[正]` implicit_cast 仅允许隐式可转类型、is_one_of 真/假（static_assert）
- `[界]` 基类契约：copyable 可拷贝、noncopyable 不可拷贝/移动、less_than_comparable 由 operator< 导出 >/>=/\<=（static_assert；运行时复验借 TimestampTest）

#### MacroTest（全新）

- `[正]` LIKELY/UNLIKELY 值透传、CONSTEVAL/GET_CONSTEVAL 求值
- `[正]` CASE_STRING_BEGIN/CASE_STRING/END 对枚举与 default 的映射
- `[错]` ASSERT(false) 打印后 abort——EXPECT_DEATH；ASSERT(true) 存活
- `[界]` ATTR_\* 纯编译期宏以"编译通过即通过"覆盖，不设运行时用例

#### ViewTest（全新）

- `[正]` reference_wrapper 隐式构造、Peer()/NativeHandle() 指向原对象
- `[界]` 拷贝与移动后仍视图同一对象
- `[命]` 不拥有对象语义（文档化 + 简单用例）

#### SingletonTest（全新）

- `[正]` 同类型多次 getInstance 返回同一地址；不同类型实例互异；构造/析构 deleted（static_assert）
- `[并]` 8 线程并发首次 getInstance，全部同一实例且仅构造一次（构造计数器）

#### BlockingQueueTest（迁移 5 + 新增）

- 迁移：put/take、阻塞唤醒、drain、over 解除阻塞、DISABLED_MoveOnlyElements（D23）
- `现状锁定·D1`：`BoundBlockingQueueTest.ConstructorThrowsWhileD1Unfixed`（启用，EXPECT_ANY_THROW 正容量构造）
- `新增·D1 阻塞`：BoundBlockingQueue 正向场景集（有界阻塞 put、isFull、drain、over）以 DISABLED 预留
- 新增：
  - `[正]` FIFO 严格顺序（N 元素 take 顺序一致，现状 PutTake 未显式断言）
  - `[并]` 4 生产者×4 消费者交换 10000 元素总数守恒
  - `[界]` over() 后 put 仍成功（现状语义锁定，疑点 Q6）
  - `[界]` over() 后 take 排空剩余再返回 T()

### 2.3 test/exception/

#### ExceptionTest（全新）

- `[正]` 各异常类型默认构造与定制 what() 文案
- `[正]` 类型层次：ConfigUnLoad→std::exception，其余→std::logic_error（static_assert + 多态 catch）
- `[界]` 拷贝与抛出/捕获往返 what() 稳定

### 2.4 test/log/

#### LogFormatterTest（迁移 2 + 新增 4）

- 迁移：FormatterMessageAndNewline、FormatterPlaceholders
- 新增：
  - `[正]` 全占位符逐项断言：%m %p %r %c %t %d{…} %f %l %M %T %F %% %h %P（间接覆盖 MacroReflaction 全部 FormatItem）
  - `[界]` 字面文本 suffix 穿插
  - `[界]` %d 缺省时间格式与自定义 %d{%H:%M}
  - `[错]` 非法占位符（如 %z）容错——锁现状行为并附注释

#### LogAppenderTest（迁移 2 + 新增 5）

- 迁移：FileAppenderWritesFile、AsyncFileAppenderWritesFile
- 新增：
  - `[界]` RollFile 滚动：小 rollSize 写超生成新文件（文件名含时间戳），旧文件保留
  - `[正]` SyncAppendFile::Flush 后内容立即可见（WrittenBytes 守恒）
  - `[并]` 8 线程并发 Append N 条，落盘总行数守恒
  - `[命]` AsynAppendFile：start→Append→stop 排空队列无丢日志
  - `[界]` StdoutLogAppender 冒烟（不断言 stdout 内容）

#### LoggingTest（拆自 LogTest + 新增 3）

- `[正]` Logging 析构时才提交流内容（构造后未析构前 appender 无内容）——RAII 提交契约，零覆盖
- `[正]` LogContext 字段填充（file/line/func/level/loggerName）
- `[界]` LogLevel 枚举序关系与 NUM_LOG_LEVELS

#### LoggerTest（迁移 5 + 新增 5）

- 迁移：LevelAndName、StreamMacroWritesToAppender、LevelFiltering、FormatMacro、DelAppender、DISABLED_FormatterAppliedByLogger（D22）
- 新增：
  - `[正]` LOG_FMT_\* 与流式输出等价
  - `[正]` 多 appender 广播：同一条日志两个 CaptureAppender 都收到
  - `[界]` delAppender 后追加日志不再写入已删 appender
  - `[错]` CHECK_NOTNULL(空)：记录 FATAL 并返回原指针（不抛不断——现状锁定）；ASSERT_THROW 条件假时抛出并记录；LOG_THROW 抛出并记录
  - `[并]` setFormatter 与 LOG 写入并发（RWLock 路径）

#### LoggerManagerTest（迁移 1 + 新增 3）

- 迁移：LoggerManagerReturnsSameInstance
- 新增：
  - `[正]` getLogger 同名复用、新名新建
  - `[正]` LoadConfig 从 sys_config 读取 logger 数组装配（TempDir+toml；覆盖 name/path/EnableStdout/format/level 字段，Logger.cc 装配逻辑零覆盖）
  - `[界]` LoadConfig 对已存在 logger 的更新路径
  - `[命]` LoadConfig 后恢复 sys_config 状态（目标内无全局污染）

### 2.5 test/net/（本方案核心）

#### AddressTest（迁移 8 + 新增 4）

- 迁移：IPv4Fields、IPv6Fields、WildcardAddress、DISABLED_InvalidIpThrows（D20）、InvalidIpCurrentlyParsesAsAny（现状锁定，与 D20 并存）、DISABLED_IpStringHasNoPadding（D17）、DISABLED_ToStringHasColon（O3）、SockaddrCast；D17 的 c_str() 截断绕行保留
- 新增：
  - `[界]` 端口边界 0 与 65535 的 port() 往返
  - `[界]` `Address("::1", port, true)` IPv6 字符串构造
  - `[正]` Length() 对 v4/v6 的 sizeof 契约
  - `[界]` 默认构造 Address 的 family()/getSockAddr() 现状锁定（疑点 Q7）

#### SocketTest（迁移 8 + 新增 8）

- 迁移：socketpair 读写、VectoredIO（writev/readv）、ShutdownWrite 半关闭、非阻塞 EAGAIN、TCP bind-listen-accept-connect 全链路、TCP 选项、DISABLED_SetNonBlockAndCloseOnExecSetsFdCloexec（D16）、DISABLED_GetSocketErrnoReportsSoError（D15）
- 新增：
  - `[命]` 析构关闭 fd：作用域结束后 FdCount 恢复（RAII 核心契约，零直接断言）
  - `[界]` Socket(INVALID_SOCKET) 构造后 isAvailable()==false；close() 后 fd 置无效
  - `[错]` 二次 close 触发 assert（死亡测试，以 src/net/Socket.cc:141-150 实现为准；疑点 Q12）
  - `[错]` connect 未监听端口返回 false（不抛）
  - `[界]` 非阻塞 listen fd 上 accept 无连接立即返回 INVALID_SOCKET
  - `[正]` 已连接 socket 的 getLocalAddr/getRemoteAddr 端口正确（注意 D17 绕行）
  - `[正]` setReuseAddr/setReuseExclusiveAddr/setKeepAlive/setTcpNoDelay(true/false) 返回 true
  - `[正]` read(Buffer\*) 写入可写区后的 ReadableSize（与 Buffer 单元衔接）
  - `[界]` Family::UNIX + Type::TCP 构造冒烟（AF_UNIX stream 创建成功）

#### ChannelTest（迁移 6 + 新增 4）

- 迁移：fixture(LoopThread) 6 用例整组（回调分派/无回调不崩/Enable-Disable 组合/回调内移除/1000 次建删压测/128 channel×4 writer 并发）
- 新增：
  - `[错]` kError 事件分派到 ErrorCallback（src/net/Channel.cc 三分支中 error 分支零覆盖）
  - `[正]` kReadable|kWritable 同时就绪时按读→写顺序双回调
  - `[界]` ListeningEvent/OccurEvent/fd()/OwnerLoop() getter 契约
  - `[界]` DisableAllEvent→再 EnableReadEvent 复用同一 Channel
  - `[命]` `新增·D27`：DISABLED_DestroyDuringDispatch——事件分派回调中析构 Channel → heap-use-after-free 回归锁定（TSAN 可复现）
  - `[压]` 多 writer 并发写加深（128 channel × 8 writer × 1000 写）

#### EventDispatcherTest（新单元；D14 迁入 + 新增 8）

可测性：EventDispatcher 构造仅保存 loop 指针，Channel::update 经 loop->UpdateChannel 断言 isInLoopThread——因此在**创建 EventLoop 的同线程**栈上直接构造 dispatcher + eventfd + Channel，手动调 `Dispatcher(0, list)` 轮询，无需启动 Loop()。

- `[正]` UpdateChannel(EnableRead) 后 eventfd 写 8 字节，Dispatcher(0) 返回该 Channel 且 OccurEvent 含 kReadable
- `[界]` timeoutMs=0 无事件立即返回空列表
- `[界]` timeoutMs>0 精度容差（50ms 档）
- `[正]` Modify 路径：EnableRead→EnableWrite 再 Dispatcher（EPOLL_CTL_MOD 分支）
- `[正]` RemoveChannel（经 DisableAllEvent）后不再上报
- `[界]` 事件列表扩容：>16 个 channel 同时就绪（kInitEventListSize=16 的 resize 分支）
- `[错]` EventType::kReadable == EPOLLIN|EPOLLPRI 契约断言
- `[压]` 1000 channel 注册/修改/移除循环
- `[命]` 迁移·D14：DISABLED_DestructorClosesEpollFd（原 EventLoopLifecycleTest.DISABLED_LoopLifecycleFdLeak，FdCount 断言）
- `[界]` epoll_create1 失败路径不可注入——文件头文档说明（疑点 Q13）

#### EventLoopTest（迁移 7 + 新增 5）

- 迁移：IsInLoopThread、RunInLoopInlineInLoopThread、RunInLoopPreservesOrder、RunInLoopTaskStorm、WakeupStorm、LoopCanBeCreatedAndDestroyed；D14 用例外迁至 EventDispatcherTest
- 新增：
  - `[正]` RunInLoop vs QueueInLoop 可观测区分：同线程调用立即执行、跨线程排队到下轮 doPendingFunctor（现状顺序性用例未显式区分两条路径）
  - `[界]` 嵌套 RunInLoop：任务内再 RunInLoop 同线程内联执行（不重入死锁）
  - `[界]` Stop() 后 RunInLoop 投递任务的行为（现状锁定，疑点 Q4）
  - `[界]` 同线程第二个 EventLoop 构造触发 assert（t_ThisThreadLoop 检查，死亡测试）
  - `[错]` pending functor 抛异常 → 异常逃逸 Loop()（doPendingFunctor 无 try/catch——现状锁定 + 疑点 Q3）
  - `[并]` 16 线程 RunInLoop/QueueInLoop 混合风暴（现有 8×5000 单一方式）
  - `[并]` Wakeup 与 Stop 并发竞争
  - `[命]` 析构未运行的 EventLoop：wakeup fd 关闭（D14 修复后强断言）

#### ServerStatusTest（迁移 2 + 新增 2）

- 迁移：DefaultStatus、DISABLED_SetStatusReturnsStatus（D5）
- 新增：
  - `[正]` 状态机全流转 Null→Initializing→…→Destroy 逐档 setStatus/getStatus
  - `[正]` StatusToString 全部 7 个枚举值字符串
- 注意（写入文件头）：ServerStatus 不在 namespace bus（全局模板）；isAtomic 模板参数当前无效（疑点 Q5）

#### ListenerTest（迁移 4 + 新增 2）

- 迁移：AcceptSingleConnection、ConcurrentAccept（8×8）、NoCallbackClosesAcceptedConnection、DISABLED_DestructorReleasesFd（D18）；3 处有意泄漏统一为标准 TODO(D18) 注释格式
- 新增：
  - `[错]` 端口被占用时 Listen 行为（bind 失败路径，现状锁定）
  - `[界]` Listen() 二次调用幂等/报错（现状锁定）
  - `[压]` accept 风暴加深：16×16 并发 + fd 唯一性
  - `[命]`（D18 修复后）非泄漏路径析构释放 fd

#### TCPConnectionTest（迁移 7；新增多被 D11 阻塞）

- 迁移：ConstructAndDestruct、CloseTransitionsToDisconnected、CallbacksCanBeSet、SendIsIgnoredWhenDisconnected、DISABLED_SendFromNonLoopThreadIsSafe（D9+D11）、DISABLED_SendBuffer（D10）、DISABLED_ConnectEstablishedEcho（D11，GTEST_SKIP 占位）
- 新增（可测部分）：
  - `[正]` name/localAddr/remoteAddr/fd getter 契约
  - `[界]` close() 幂等（现状锁定）
  - `[错]` socketpair 对端关闭触发 handleClose 路径中可观测部分（以 Channel kError→ErrorCallback 路径为准）
- 预留（D11 修复后启用，全部登记 known-issues）：
  - `[正]` connectEstablished 后 Connected 状态 + 读事件启用 + MessageCallback
  - `[正]` 对端关闭 handleClose → Disconnected + CloseCallback
  - `[界]` send 大数据（>SO_SNDBUF）分块 + WriteCompleteCallback
  - `[界]` HighWaterMarkCallback 超阈值触发一次
  - `[并]` 非 loop 线程 send 数据完整（D9）

### 2.6 test/thread/

#### MutexTest（迁移 2 + 新增 3）

- 迁移：MutexProtectsCounter（8×10000）、RWLockProtectsCounter（8×1000）
- 新增：
  - `[正]` MutexLockGuard 构造加锁析构解锁（外部观测：guard 存活时另一线程 lock 阻塞，用 WaitFor）
  - `[正]` ReadLockGuard 多读者并发持有（计数器）与 WriteLockGuard 独占
  - `[界]` getNativeHandle() 非空
  - `[界]` 无名 guard 防呆宏（编译期契约文档化）
  - `[界]` MutexLock 非递归：同线程二次 lock 死锁——不写用例，文件头 attention 说明

#### ConditionTest（迁移 1 + D4 + 新增 2）

- 迁移：ConditionNotify、DISABLED_TimeWaitReturnsFalseOnTimeout（D4）
- 新增：
  - `[正]` notifyAll 唤醒全部 N 个等待者
  - `[界]` TimeWait 超时路径现状行为锁定（返回 true，D4 现状半边）
  - `[正]` TimeWait 窗口期内被 notify 的返回值（现状锁定，D4 修复后合并断言）

#### SemaphoreTest（迁移 1 + 新增 2）

- 迁移：SemaphoreSync
- 新增：`[界]` 初值 2：两线程不阻塞第三阻塞；`[并]` N notify 对 N waiter 一一放行守恒

#### CountDownLatchTest（迁移 1 + 新增 2）

- 迁移：CountDownLatchSync
- 新增：`[界]` count=0 构造后 wait() 直通；`[并]` 8 waiter + 8 次 coutDown（方法名按现拼写，疑点 Q8）；`[正]` getCount 递减观测

#### ThreadTest（Thread+CurrentThread；迁移 2 + 新增 5）

- 迁移：RunJoinAndThreadInfo、BindCpu
- 新增：
  - `[正]` CurrentThread 套件：主线程 tid()==getpid()、isMainThread()==true、ThreadName 默认值；子线程内 isMainThread()==false、getCPU() 合法（零直接覆盖）
  - `[界]` 未 run() 即析构（现状锁定，以 Thread.cc 为准）
  - `[界]` join 后线程函数已结束；未 join 析构行为（现状锁定）
  - `[界]` bindCPU 越界核心号（现状锁定）
  - `[正]` 默认线程名
  - `[并]` 100 线程并发启动各自 latch 同步后 tid 互异

#### ThreadPoolTest（迁移 2 + 新增 4）

- 迁移：ExecutesAllTasks、RunWithoutStartExecutesInline
- 新增：
  - `[界]` stop() 后 run() 行为（现状锁定）
  - `[界]` start() 二次调用幂等
  - `[并]` start(1) 单 worker 串行化（顺序断言）
  - `[并]` 任务抛异常后 worker 存活（现状锁定，疑点 Q9）
  - `[压]` 8 线程×1000 任务计数守恒

#### ProcessInfoTest（迁移 1 + 新增 1）

- 迁移：UtilTest.ProcessInfo
- 新增：`[正]` Pid()==::getpid() 同进程稳定；hostname() 非空不含 '/'

### 2.7 test/time/

#### TimestampTest（迁移 6 + 新增 5）

- 迁移：NowAndUnits、LiteralAndArithmetic、FormatUtc、DayBoundaries、ToPosixTypes、DISABLED_TimespecNanoseconds（D24）
- 新增：
  - `[正]` less_than_comparable 导出 >/>=/\<= 全组合；==/!= 未定义以 static_assert 锁定（疑点 Q10）
  - `[界]` 负数 epoch 的 toDay/nextDay（C++ 整除向零取整，实测；错位则登记新缺陷，疑点 Q11）
  - `[正]` now() 单调不回退
  - `[界]` toTimeval tv_usec 截断 0-999999（D24 修复后 tv_nsec 同档）
  - `[界]` 恰好午夜整（86400s 整数倍）的 toDay/nextDay
  - `[正]` operator\<\< 输出与 _stamp 字面量流断言

#### DateTimeTest（迁移 6 + 新增 5）

- 迁移：ValidDateAndLeapYear、InvalidDateThrows、ValidAndInvalidTime、FormatAndValidation（TZ）、OutputContainsComponents、SpaceSignCanBeSwitched（fixture TearDown 的 revertSign 恢复保留）
- 新增：
  - `[界]` 闰年三例：2000（世纪闰）、1900（非闰，2/29 抛）、2024（闰）
  - `[界]` 各月天数边界（1/31、4/30、12/31）
  - `[界]` Time 边界 23:59:59.999999999 与各字段越界抛（hour=24、minute=60、second=60、nano=1e9）
  - `[界]` TimeZoneOffset ±12:00 合法、±12:01/±13:00 抛
  - `[正]` DateTime 默认 TZ 偏移构造输出格式

### 2.8 test/util/

#### TimeUtilTest（迁移 2 + 新增）

- 迁移：CustomEpoch、GetElapse
- 新增：`[界]` epoch 常量精确断言（1735660800000000 μs == 2025-01-01T00:00:00Z，防手改）；`[正]` getElapse 单调

#### StringUtilTest（迁移 2 + 新增 1）

- 迁移：BaseFileName、StrToFileSize、DISABLED_StrToFileSizeKB（D21）
- 新增：`[界]` BaseFileName 无斜杠/多斜杠/尾斜杠

#### SystemUtilTest（迁移 2 + 新增 3）

- 迁移：CoreNum、SnowflakeKeyUnique
- 新增：
  - `[正]` Backtrace(64,1) 非空含当前函数符号；BacktraceToString prefix 逐行（零覆盖）
  - `[并]` 8 线程×1000 generateSnowflakeKey 全局唯一
  - `[界]` getCoreNum>=1 两次一致；getWorkId()==1 契约锁定
  - `[错]` Abort() 不写用例（core dump 打断调试体验），文件头 attention 说明

### 2.9 test/integration/

#### NetworkStressTest（迁移 7 + 新增 5 高并发专项）

迁移：EchoSingleClient、EchoConcurrentClients（32×500）、EchoLargePayload（1MB）、ConnectionChurn（8×25）、HalfClose、PipelinedMessages（8×200）、FullDuplexConcurrentReadWrite（16×256KB）；TearDown 的 fixture 级 fd 泄漏断言保留。

新增（高并发/内存安全专项）：

| 用例 | 场景 | 排查目标 |
|---|---|---|
| EchoChurnWithPendingSend | 连接风暴 + 未排空发送缓冲时断连 | 输出缓冲持有路径内存安全 |
| EchoAsymmetric | 大写小读不对称负载 | Buffer 增长/压实路径 |
| TimerAndIOInterleave | 定时器任务与连接回调同 loop 交织 | Timer/Channel 共存分派安全 |
| LongRunSoak | 混合负载持续浸泡（实施取 5s，CI 时长权衡，语义保留） | 慢性泄漏/fd 累积（TSAN/ASAN 下跑） |
| MultiLoopClientStorm | 多客户端 loop 压服务端单 loop | 跨 loop 任务队列竞争 |

### 2.10 空壳模块预留场景清单（不写码）

| 预留单元 | 触发条件 | 场景要点 |
|---|---|---|
| net/TcpServerTest | TcpServer 实现（现仅 8 行构造） | Start/Stop 生命周期、ConnectionCallback/MessageCallback 装配、thread_num=0/1/N、Stop 的 DoneCallback、重复 Start、端口占用 |
| net/EventLoopThreadPoolTest | 类体实现（现为空类） | 线程数、getNextLoop 轮询、baseloop 归属、启停 |
| HookTest | Hook 实现 | 按协程 hook 语义补 |
| net/SocketTest UDP 扩展 | UDP API 存在（现仅枚举预留） | UDP 创建/bind/收发、NIO 两种类型非阻塞标记 |

---

## 3. 网络模块高并发与内存安全排查方案（本方案核心）

### 3.1 排查双主线

- **线程安全**：TSAN 全覆盖网络路径目标（数据竞争、锁序、UAF）
- **内存安全**：ASAN（越界、UAF）+ 泄漏检测（ASAN detect_leaks，除 NO_LEAK_CHECK 目标外全开——严于现状全局关闭）+ FdCount 前后差值断言（fd 泄漏）

### 3.2 阶段门槛

每个网络阶段（P1–P5）完成定义 = ctest 全绿 **且** 该阶段目标在 build-tsan 与 build-asan 下通过。TSAN 运行需 `setarch $(uname -m) -R`（GCC 13 环境限制）。

### 3.3 发现处理流程

1. 新用例对现状源码 FAIL / TSAN/ASAN 报告问题 → 确认可复现（带证据：报告摘要/种子/序号）
2. 登记 docs/known-issues.md 第 2 节（D28+ 编号），附复现用例与源码位置
3. 测试侧固化：DISABLED 用例（回归锁定）或现状锁定用例（启用但断言当前行为）
4. 源码不动（修复另立任务，本计划只登记）

### 3.4 全量专项（P11）

36 目标 × {TSAN, ASAN} 矩阵；压测类 `--repeat until-fail:10`；结果全部记入 known-issues.md（通过项记一行汇总，问题项逐条登记）。

---

## 4. CMake 与目录方案

### 4.1 目录结构（终态）

```
test/
├── CMakeLists.txt            # 单文件注册全部 36 目标，按模块分节
├── TestHelper.h              # 通用：TempDir/CaptureAppender/FdCount/GetFreePort/ScopedFd/WaitFor + sanitizer 守卫宏
├── EventLoopTestHelper.h     # LoopThread/RunInLoopSync
├── SliceTest.cc  BufferTest.cc  ConfigTest.cc  TimerTest.cc
├── base/ exception/ log/ net/ thread/ time/ util/   # 各单元 .cc
└── integration/  EchoTestHelper.h  NetworkStressTest.cc
```

### 4.2 CMake 决策

- **单文件 CMakeLists**（不建子目录级）：codebus_add_test 是唯一注册入口，分节注释导航；子目录级会引入函数重复定义或传播问题
- **include 路径**：函数内 `target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})`，子目录源文件统一 `#include "TestHelper.h"` / `"EventLoopTestHelper.h"` / `"integration/EchoTestHelper.h"`
- **TIMEOUT 分级**：默认 60；TimerTest/EventLoopTest 120；NetworkStressTest 180（LongRunSoak 类 300）
- **NO_LEAK_CHECK**（实施修正）：ListenerTest、NetworkStressTest（D18/D27 绕行）、TimerTest（D6 绕行）、ConfigTest（D34 tomlc17 泄漏）——设置 `ENVIRONMENT ASAN_OPTIONS=detect_leaks=0`；每消除一个绕行/缺陷即移除该属性
- 顶层与 src 的 CMakeLists **不改**

函数模板：

```cmake
function(codebus_add_test TEST_NAME TEST_SOURCE)
    set(options NO_LEAK_CHECK)
    set(oneValueArgs TIMEOUT)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${TEST_NAME} codebus gtest_main gtest pthread)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
    if(ARG_NO_LEAK_CHECK)
        set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
    endif()
endfunction()
```

---

## 5. Helper 重组

| 现文件 | 去向 | 说明 |
|---|---|---|
| test/TestHelper.h | 原地保留 + sanitizer 守卫宏 | TempDir/CaptureAppender/FdCount/GetFreePort/ScopedFd/WaitFor 通用；补文件头格式 |
| test/NetworkTestHelper.h | **拆分后删除** | LoopThread/RunInLoopSync → test/EventLoopTestHelper.h（供 net/\*、TimerTest 用）；EchoServer/EchoClient → test/integration/EchoTestHelper.h（补 `#include <cassert>`——现状用了 assert 未显式包含） |

**EchoServer 三处绕缺陷注释迁移跟踪**（随代码迁入 EchoTestHelper.h，统一 TODO(Dxx) 格式，纳入 known-issues.md「绕行位置」列）：

1. Connection::output 用 string 发送队列绕 D13 → 注释迁至 struct Connection 处
2. m_listener.release() 有意泄漏绕 D18 → 注释迁至 Shutdown() 内
3. m_zombies 僵尸连接列表绕 D27（EventLoop 分派器不保护活动 Channel 存活）→ 注释迁至 CloseConnection()

跟踪机制：每个绕行点 (a) 带 TODO(Dxx) 可 grep；(b) known-issues.md 登记 文件+函数；(c) 缺陷修复验收 = 去 DISABLED 前缀 + 删绕行 + ctest 全绿 三动作。

---

## 6. 缺陷登记方案

完整登记表见 [docs/known-issues.md](../known-issues.md)（活文档，随实施持续更新）。规则：

- 编号只增不改不复用；D12/D25/D26 语义不可考（引用它们的文档不存在），永久空缺并注明
- 新缺陷自 D27 起（D27 = EventLoop 分派循环不保护活动 Channel 存活，TSAN 已复现）
- 用例迁移 D 编号不变；用例名允许精化（原名/新名成对记录于对照表）
- 每条登记：源码位置 / 用例位置 / 绕行位置 / 启用条件（三动作）

迁移对照表见 known-issues.md 第 1 节。

---

## 7. 命名与风格约定

- 4 空格缩进（TAB 零容忍）、遵循 `.claude/rules/code_style.md`
- 文件头模板：

```cpp
//***********************************************
// name      : MutexTest.cc
// brief     : thread 模块单元测试：被测 src/thread/Mutex.hpp（MutexLock/guards/RWLock）
// attention : MutexLock 非递归，同线程二次 lock 死锁不做用例；本目标无 DISABLED 用例
//***********************************************
```

- 目标名 = `<被测头文件去扩展名>Test`；套件名 = `<被测类名>Test`（一目标多套件时套件与类一一对应）；用例名 = 驼峰行为短语
- DISABLED 标准格式：

```cpp
// TODO(D4): Condition::TimeWait 返回 ETIMEDOUT==wait 结果，语义反转
// （src/thread/Condition.hpp:31-37），修复后去掉 DISABLED_ 前缀启用本用例。
TEST(ConditionTest, DISABLED_TimeWaitReturnsFalseOnTimeout)
```

- 现状锁定格式：`// TODO(Dxx): <缺陷>；本用例锁定当前行为，源码修复后改为正向断言。`
- 有意泄漏格式：`// TODO(Dxx): 有意泄漏 <对象>（<原因>），修复后改为 <reset()/栈对象>。`
- 禁用用例体内禁止留空：能编译的写完整断言；不可达路径保留 GTEST_SKIP 占位并在对照表说明
- 死亡测试统一包 sanitizer 守卫（TestHelper.h 提供宏，sanitizer 构建下 GTEST_SKIP）

---

## 8. 实施计划

> 网络路径优先（P1–P5），随后其余模块；每阶段两段式变更（纯迁移段 + 新增场景段）；ctest 全绿（网络阶段追加 sanitizer）才进下一阶段。**删除旧文件属红线操作：每批删除前向用户展示清单并确认。** 不自动 git commit（CLAUDE.md），每阶段完成后展示变更摘要供手动提交。

| 阶段 | 交付物 | 删除旧文件 | 累计目标 |
|---|---|---|---|
| P-1 | 本设计文档 + docs/known-issues.md + docs/README.md 索引 | — | — |
| P0 | 目录骨架 + CMake 函数改造（向后兼容）+ EventLoopTestHelper.h 拆出 + TestHelper.h 守卫宏（新旧并行） | — | 15 |
| P1 | SliceTest + BufferTest（内存安全核心，ASAN 门槛建立） | test/BufferTest.cc | 16 |
| P2 | net 前半：Address/Socket/ServerStatus/Channel（含 D27 回归用例） | test/AddressTest.cc、SocketTest.cc、ChannelTest.cc | 17 |
| P3 | net 核心：EventDispatcherTest（新单元+D14）、EventLoopTest（高并发深化） | test/EventLoopTest.cc | 18 |
| P4 | net 连接层：ListenerTest、TCPConnectionTest | test/ListenerTest.cc、TCPConnectionTest.cc | 18 |
| P5 | TimerTest（并发加深） | test/TimerTest.cc | 18 |
| P6 | base 5 + exception 1 | test/BlockingQueueTest.cc | 23 |
| P7 | time 2 + util 3 | test/TimeTest.cc、UtilTest.cc | 26 |
| P8 | thread 7 | test/ThreadTest.cc | 32 |
| P9 | log 5 | test/LogTest.cc | 36 |
| P10 | ConfigTest | test/ConfigTest.cc | 36 |
| P11 | integration：EchoTestHelper.h + NetworkStressTest 扩展（5 新用例）+ 删 NetworkTestHelper.h + 全量 sanitizer 专项 | test/NetworkTestHelper.h、NetworkStressTest.cc（迁 integration/） | 36 |
| P12 | 终核 + CLAUDE.md/README/docs 索引同步 | — | 36 |

同名目标冲突：新目标与旧目标同名时（12 对），旧文件在该阶段新目标全绿后同批删除。

### 8.1 每阶段验证基线

```bash
cd build && cmake .. && make -j$(nproc)
ctest --output-on-failure
ctest -N | tail -n 1               # 目标数对照上表
```

网络阶段（P1–P5）追加：

```bash
cmake -S . -B build-tsan -DCODEBUS_SANITIZER=thread -DCMAKE_BUILD_TYPE=Debug && cmake --build build-tsan -j$(nproc)
setarch $(uname -m) -R ctest --test-dir build-tsan -R "<本阶段目标正则>" --output-on-failure
cmake -S . -B build-asan -DCODEBUS_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan -R "<本阶段目标正则>" --output-on-failure
```

P12 终核：

```bash
ctest --repeat until-fail:10 -R "NetworkStress|EventLoop|Channel|Listener|Timer"
grep -rhoE "TODO\((D[0-9]+(/[0-9]+)*|O3)\)" test/ | sort | uniq -c   # D 编号无丢失
ctest -N | tail -n 1                                                # = 36
```

迁移段行为不变证明：迁移前后各跑一次 `ctest -R <目标>` 对比用例数与通过数。

---

## 9. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| R1 | 迁移中行为漂移 | 纯迁移段 diff 仅文件头与 include；前后 ctest -R 对比 |
| R2 | 有意泄漏在 ASAN 误报 | NO_LEAK_CHECK 仅限 ListenerTest/NetworkStressTest；消除绕行即移除 |
| R3 | 压测/TSAN 超时 | TIMEOUT 分级 60/120/180/300 |
| R4 | TSAN 已知 UAF（D27）波及现有用例 | 迁移时核实 RemoveChannelInCallback 等在 TSAN 下现状，报错列 known-failure 标 D27，不静默放行 |
| R5 | 死亡测试在 sanitizer 不稳定 | 守卫宏，sanitizer 构建下 GTEST_SKIP |
| R6 | 高并发用例 flaky | WaitFor 显式超时、失败输出种子/序号；新并发用例 --repeat until-fail:10 验证后入库 |
| R7 | D11 阻塞 TCPConnection 深度 | 场景清单全部入 known-issues，DISABLED 预留，修复即启用 |
| R8 | 全局状态污染（LoggerManager/sys_config/s_sign） | 目标进程级隔离 + 目标内 fixture TearDown 恢复 |
| R9 | FdCount 脆弱 | 只断言前后差值；异步日志先 stop 排空再计数 |
| R10 | EventDispatcherTest 绕过 EventLoop 断言链 | 用例在创建 EventLoop 同线程执行；文件头注明间接路径归 EventLoopTest |
| R11 | 新发现问题数量不可预估 | known-issues.md 活文档按阶段增量更新；只登记不修复 |
