# 项目名称
codebus
# 项目概述
- 项目是一个基于C++20的高性能服务器框架库，包含以下几个模块：
  - 网络模块：实现网络通信的功能，包括TCP、UDP等协议（UDP 待实现）。
  - 事件模块：基于epoll等事件驱动机制，实现高效的事件处理。
  - 线程模块：封装线程相关的功能，包括线程池、线程同步等。
  - 定时器模块：实现定时器功能，用于定时任务。
  - 日志模块：实现日志功能，用于记录系统运行时的信息。
  - 配置模块：实现配置文件的读取和解析功能。
  - 内存模块：实现内存管理功能，包括内存池、内存分配等（待实现）。
  - lua模块：实现lua脚本的执行功能，用于扩展项目的功能和业务逻辑（待实现）。
  - 其他模块：根据项目需求，可能还会包含其他模块，如数据库模块、缓存模块等（待实现）。
# 技术栈
- Backend: C++，lua
- Database: mysql,redis
# Role
C++开发工程师，lua开发工程师，架构师，项目负责人，测试专家
# 主要目标
- 你是一个资深C++开发专家、lua开发工程师、架构师，现在正在开发一个C++网络框架库，你需要了解项目结构、目标、架构、实现方式等，确保项目的质量和可维护性，同时考虑到项目的可扩展性和性能。从而实现一个稳定、高效、可扩展的网络框架库。

# 规则和要求
- 所有生成的文档都需要按结构放在“docs”目录下，你需要按照需求设计来组织些文档，确保用户可以轻松理解和使用这些功能。
- 当用户向你提出任何需求时，你需要先理解现有项目的结构和代码，确保你生成的代码和文档都符合项目的目标和架构，在文档中清晰的描述所有功能的用途、使用方法、参数说明、返回值说明等，确保用户可以轻松理解和使用这些功能。
- 生成的所有代码和文档都需要基于本项目现有的代码风格和结构。
- 你需要总结整个项目，并将项目说明写到README.md文件中，该文档是github中的项目说明。
- 当前网络框架库参考了muduo和sylar框架，请熟悉这两个框架的代码结构，以便我进行开发。
- 你生成的所有文档都需要符合github风格的形式来书写。
- 你所有生成的代码都需要符合C++20标准，代码风格统一，符合现代C++最佳实践。
- 当任务完成时，你需要告诉用户任务完成的情况并附证据和文件链接。
- 需要提供详细注释和文档说明，注释包含每个接口的作用、出入参和异常，格式必须统一有条理。
- 当用户向你提出需求时，当该需求涉及到改动代码时，你需要提供设计文档和计划，并将文档存放在“docs/spec”目录下。文档中要详细描述任务的需求、方案设计、改动点和计划。
- 每完成一个任务时，需要将任务拆分成任务单元，并对各个单元进行单元测试
- 当你需要向用户提供多个选择项时，你需要告诉用户需求是什么或者问题是什么，并且告诉用户各方案的差异和代码改动的对比

# Git
- 禁止自动git commit和git push，除非我明确要求
- 提交前需要展示提交变更的摘要，且需要我手动确认后才能提交
- 提交时需要按模块提交，commit massage需要清晰简洁的表达出本次改动的核心

# 红线操作
以下操作即使在auto-accept模式下也必须先问我
 - 删除文件、目录或git历史
 - 修改.env、密钥、token、证书、CI/CD配置
 - git push、git rebase、git reset --hard、强制推送
 - 公开发布(npm publish、生产部署等)

# 编译和测试

## 编译命令

```bash
# 配置（默认 Debug 模式）
mkdir build && cd build
cmake ..

# 配置 Release 模式
cmake -DCMAKE_BUILD_TYPE=Release ..

# 编译（使用所有可用核心）
make -j$(nproc)
```

编译输出：
- 库文件：`../lib/`（`libcodebus.so` 动态库、`libtomlc17.a` 静态库）
- 可执行文件：`../bin/`

## 运行测试

测试位于 `test/` 目录，使用 gtest（`3rd/gtest/`，链接 `gtest_main`）并接入 CTest。**一单元一测试目标**（目录镜像 src/ 结构，共 41 个目标）：

- 根级（镜像 src 根级源文件）：`SliceTest`、`BufferTest`、`ConfigTest`、`TimerTest`
- `base/`：`TypeTest`、`MacroTest`、`ViewTest`、`SingletonTest`、`BlockingQueueTest`
- `exception/`：`ExceptionTest`
- `log/`：`LogFormatterTest`、`LogAppenderTest`、`LoggingTest`、`LoggerTest`、`LoggerManagerTest`
- `time/`：`TimestampTest`、`DateTimeTest`
- `util/`：`TimeUtilTest`、`StringUtilTest`、`SystemUtilTest`
- `thread/`：`MutexTest`、`ConditionTest`、`SemaphoreTest`、`CountDownLatchTest`、`ThreadTest`、`ThreadPoolTest`、`ProcessInfoTest`
- `net/`：`AddressTest`、`SocketTest`、`ServerStatusTest`、`ChannelTest`、`EventDispatcherTest`、`EventLoopTest`、`EventLoopThreadTest`、`EventLoopThreadPoolTest`、`ListenerTest`、`TCPConnectionTest`、`TcpServerTest`、`ConnectorTest`、`TcpClientTest`
- `integration/`：`NetworkStressTest`（多组件协同压测，EchoServer 为 TcpServer 装配：echo/风暴/半关闭/全双工/定时器交织/浸泡/多 loop）

```bash
cd build
cmake ..
make -j$(nproc)

ctest --output-on-failure                 # 全部测试
ctest -R BufferTest                       # 指定单元
ctest --repeat until-fail:10 -R "NetworkStress|EventLoop|Channel|Listener|Timer"  # 并发稳定性
```

### Sanitizer（数据竞争/内存错误检测）

```bash
# ThreadSanitizer（GCC 13 环境需关闭 ASLR 运行：setarch $(uname -m) -R；
# EventLoopTest 含 fork 型死亡测试，须加 TSAN_OPTIONS=die_after_fork=0，
# 否则 TSAN 禁止 fork 子进程建线程导致该用例 exit 66）
cmake -S . -B build-tsan -DCODEBUS_SANITIZER=thread -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build-tsan -j$(nproc)
TSAN_OPTIONS=die_after_fork=0 setarch $(uname -m) -R ctest --test-dir build-tsan --output-on-failure

# AddressSanitizer（含绕缺陷有意泄漏的目标已在 CMake 标 NO_LEAK_CHECK 自动关闭泄漏检测，
# 其余目标泄漏检测全开——严于全局关闭）
cmake -S . -B build-asan -DCODEBUS_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure
```

@attention 所有构建目录共享项目根 `bin/`（测试可执行）与 `lib/`（`libcodebus.so`）输出——切换 sanitizer/常规构建后需强制重链（`rm bin/*Test lib/libcodebus.so && cmake --build <dir>`），否则 ctest 会跑到上一变体的二进制、或把上一变体的库链进新二进制（TSAN 库混链表现为 `FATAL: ThreadSanitizer: unexpected memory mapping`）。

已知缺陷登记（D 编号体系 + 疑点 Q 清单）统一维护在 [docs/known-issues.md](docs/known-issues.md)：用例以 `DISABLED_` 前缀 + `TODO(Dxx)` 注释对应，修复源码后按「去前缀 + 删绕行 + ctest 全绿」三动作回收。

# 依赖项

- **编译器：** GCC 7+ 或 Clang 5+（需 C++20 支持）
- **CMake：** 3.10+
- **操作系统：** Linux（POSIX 系统）
- **库：**
  - `pthread`（自动链接）
  - tomlc17（位于 `3rd/toml/`，TOML 解析）
  - gtest（位于 `3rd/gtest/`，仅测试使用）

# 核心架构设计

## 事件驱动架构（One Loop Per Thread）

- 每个 IO 线程拥有独立的 `EventLoop`
- `Channel` 封装文件描述符和事件
- `EventDispatcher`（基于 epoll）分发 I/O 事件
- 使用 `RunInLoop()` 从其他线程安全地执行任务

**关键调用链：**
```
EventLoop::Loop()
  -> EventDispatcher::Dispatcher() (epoll_wait)
  -> Channel::HandleEvent() 分发回调
  -> 执行 PendingFunctors (跨线程任务)
```

## 线程安全机制

- 所有不可复制类继承 `noncopyable` 基类
- 互斥锁使用：`MutexLock` + `MutexLockGuard`
- 读写锁使用：`RWLock` + `ReadLockGuard`/`WriteLockGuard`
- 支持 Clang 线程安全注解（需 `-Wthread-safety` 编译选项，见 `base/Macro.h`）

## 资源管理与所有权约定

- 全部使用 RAII 模式（锁、fd、智能指针）
- 所有权语义三种：
  - `View<T>`（`base/view.h`）：借用视图，不拥有生命周期（包装 `std::reference_wrapper<T>`），如 `Channel`、`Listener` 对所属 `EventLoop` 的引用
  - `std::shared_ptr`：共享所有权
  - `std::unique_ptr`：独占所有权
- 使用 `View<T>` 时须确保被引用对象生命周期长于视图本身
- 字符串参数优先使用 `std::string_view` 而非 `const std::string&`

# 重要架构要点

## 1. Channel 生命周期
- 每个 `Channel` 只属于一个 `EventLoop`（IO 线程）
- 永远不要跨线程共享 Channel
- 使用 `OwnerLoop()` 检查所属的 EventLoop
- `Channel` 不拥有 fd 的生命周期

## 2. IO 线程安全
- 使用 `EventLoop::isInLoopThread()` 检查是否在所属 IO 线程
- 使用 `RunInLoop(callback)` 安全地排队执行跨线程任务
- `Wakeup()` 通过 eventfd 唤醒阻塞的 epoll_wait

## 3. ServerStatus 模板
- 需要状态机的类继承 `ServerStatus<bool>`（位于 `net/ServerStatus.h`）
- 状态流转：Null → Initializing → Initialized → Running → Stopping → Stopped → Destroy

## 4. 单例模式
- 管理器类（如 `LoggerManager`）使用 `Singleton<T>` 模板
- 线程安全的懒加载初始化

## 5. 缓冲区设计
- `Slice`（`Buffer.hpp`）：基于 `std::span<std::byte>` 的字节视图
- `Buffer`：预留/可读/可写三区结构，滑动窗口增长
- 文件大小字面量：`4_MB`、`128_MB`（定义于 `base/type.h`）

# 模块交互关系

## 网络模块交互
```
TcpServer -> EventLoopThreadPool -> EventLoop -> EventDispatcher(epoll)
              (round-robin)         -> Channel(fd) -> TCPConnection -> 回调
```

## 配置系统
```
Config -> Handle(tomlc17 RAII 封装) -> Value(抽象基类)
                                        -> Value4Toml(具体实现)
                                        -> NullValue(键不存在时返回)
```
- 配置访问：`SYS_CONF("key")` 宏（展开为 `sys_config.get("key")`）
- 系统配置 `sys_config` 在启动时自动加载 `./sys.toml`（存在时）

## 日志系统
```
Logger -> LogFormatter -> LogAppender(控制台/文件) -> 输出
```
- 日志配置从 `sys_config` 的 `logger` 数组读取（`LoggerManager::LoadConfig`）

# 关键实现细节

## EventLoop 线程模型
- `EventLoop::Loop()` 在所属线程的 `epoll_wait()` 中阻塞
- `RunInLoop(callback)` 将任务排队到 `m_PendingFunctor`
- `Wakeup()` 通过 `m_WakeupChannel` (eventfd) 中断 epoll_wait
- `doPendingFunctor()` 在每次循环开始时执行跨线程任务

## Channel 事件处理
- 通过 `EnableReadEvent()`、`EnableWriteEvent()` 注册事件
- 使用 `setReadCallback()`、`setWriteCallback()`、`setErrorCallback()` 设置回调
- `HandleEvent()` 根据 `m_OccurEvents` 分发到相应回调
- 事件常量：`EventType::kNone`/`kReadable`/`kWritable`/`kError`

## Timer 定时器
- 基于 `timerfd` 的 `TimerManager`，挂载于所属 `EventLoop`
- 定时器按到期时间存入 `std::set`，哨兵模式处理处理期间的插入
- `addTimer(cb, when, interval)`：interval 为 0 时一次性触发，否则周期触发
- `cancel()` 为惰性标记取消

## Thread 类
- 底层使用 `pthread`
- 支持通过 `bindCPU()` 绑定 CPU 核心亲和性
- 使用 `CountDownLatch` 确保线程完全启动后 `run()` 才返回

## 配置系统注意事项
- 当前主实现为 `Config`（基于 tomlc17），旧版 `Config4Toml`/`ConfigManager` 已删除
- `Value::asXxx()` 返回 `std::optional`，类型不匹配返回 `nullopt`
- `Config` 内部使用 `std::shared_mutex` 保证线程安全

# 代码风格要求

**所有代码必须遵循 [`.claude/rules/code_style.md`](.claude/rules/code_style.md)**
（命名、文件头注释、格式、项目宏 `ATTR_*`/`LOG_*`/`ASSERT_*`、资源管理约定）。

# 待实现模块

项目目标中提及但尚未实现的模块：
- 内存模块（内存池、内存分配）
- Lua 模块（脚本执行）
- UDP 协议支持（网络模块，`Socket::Type` 已预留枚举）
- 数据库和缓存模块
- `Hook`（系统调用 Hook，协程基础设施）为空实现

# 目录结构
 - `3rd/` - 第三方库（tomlc17、gtest）
 - `bin/` - 可执行程序生成位置
 - `build/` - CMake生成脚本位置
 - `docs/` - 模块文档（`docs/README.md` 为索引），`docs/spec/` 为设计文档与计划
 - `lib/` - 动态库生成位置
 - `src/` - 源代码
  - `src/base/` - 存放全局使用的工具类（宏、基类、单例、View、阻塞队列）
  - `src/exception/` - 异常
  - `src/log/` - 日志
  - `src/net/` - 网络
  - `src/thread/` - 线程
  - `src/time/` - 时间（Timestamp、DateTime）
  - `src/util/` - 工具
  - `src/Buffer.hpp` - 缓冲区（Slice + Buffer）
  - `src/Config.h` - 配置模块
  - `src/Hook.h` - 系统调用 Hook（待实现）
  - `src/Timer.h` - 定时器模块
 - `test/` - 测试

# 参考资料

本项目设计参考：
- [muduo](https://github.com/chenshuo/muduo) - 高性能 C++ 网络库
- [sylar](https://github.com/sylar-yin/sylar) - 高性能服务器框架

<!-- superpowers-zh:begin (do not edit between these markers) -->
# Superpowers-ZH 中文增强版

本项目已安装 superpowers-zh 技能框架（21 个 skills）。

## 核心规则

1. **收到任务时，先检查是否有匹配的 skill** — 哪怕只有 1% 的可能性也要检查
2. **设计先于编码** — 收到功能需求时，先用 brainstorming skill 做需求分析
3. **测试先于实现** — 写代码前先写测试（TDD）
4. **验证先于完成** — 声称完成前必须运行验证命令

## 可用 Skills

Skills 位于 `.claude/skills/` 目录，每个 skill 有独立的 `SKILL.md` 文件。

- **brainstorming**: 在任何创造性工作之前必须使用此技能——创建功能、构建组件、添加功能或修改行为。在实现之前先探索用户意图、需求和设计。
- **chinese-code-review**: 中文 review 沟通参考——话术模板、分级标注（必须修复/建议修改/仅供参考）、国内团队常见反模式应对。仅在用户显式 /chinese-code-review 时调用，不要根据上下文自动触发。
- **chinese-commit-conventions**: 中文 commit 与 changelog 配置参考——Conventional Commits 中文适配、commitlint/husky/commitizen 中文模板、conventional-changelog 中文配置。仅在用户显式 /chinese-commit-conventions 时调用，不要根据上下文自动触发。
- **chinese-documentation**: 中文文档排版参考——中英文空格、全半角标点、术语保留、链接格式、中文文案排版指北约定。仅在用户显式 /chinese-documentation 时调用，不要根据上下文自动触发。
- **chinese-git-workflow**: 国内 Git 平台配置参考——Gitee、Coding.net、极狐 GitLab、CNB 的 SSH/HTTPS/凭据/CI 接入差异与镜像同步配置。仅在用户显式 /chinese-git-workflow 时调用，不要根据上下文自动触发。
- **diagnosing-superpowers**: 当一次 superpowers 会话出了问题、你的人类伙伴想知道原因时使用——重复劳动、无视计划、磕磕绊绊、结果质量差、某个技能没触发、"太慢了"、"为什么这么贵"、"它到底在干什么"——或者想给 superpowers 维护者整理一份 bug 报告；适用于当前会话，或按 id / 路径指定的过往会话，任何工具均可
- **dispatching-parallel-agents**: 当面对 2 个以上可以独立进行、无共享状态或顺序依赖的任务时使用
- **executing-plans**: 当你要在当前会话里亲自担任实现者执行一份实现计划时使用——你的人类伙伴选择了内联执行，或者没有可用的子智能体工具
- **finishing-a-development-branch**: 当实现完成、所有测试通过、需要决定如何集成这份工作时使用
- **mcp-builder**: 在构建 MCP 服务器或 MCP 工具时使用 —— 系统化的生产级 MCP 构建方法论，覆盖工具设计、错误处理、传输层选择与测试，让 AI 助手连接外部能力
- **receiving-code-review**: 收到代码审查反馈后、实施建议之前使用，尤其当反馈不明确或技术上有疑问时——需要技术严谨性和验证，而非敷衍附和或盲目执行
- **requesting-code-review**: 完成任务、实现重要功能或合并前使用，用于验证工作成果是否符合要求
- **subagent-driven-development**: 当在当前会话中执行包含独立任务的实现计划时使用
- **systematic-debugging**: 遇到任何 bug、测试失败或异常行为时使用，在提出修复方案之前执行
- **test-driven-development**: 在实现任何功能或修复 bug 时使用，在编写实现代码之前
- **using-git-worktrees**: 当需要开始与当前工作区隔离的功能开发，或在执行实现计划之前使用——通过原生工具或 git worktree 回退机制确保隔离工作区存在
- **using-superpowers**: 在开始任何对话时使用——确立如何查找和使用技能，要求在任何响应（包括澄清性问题）之前调用 Skill 工具
- **verification-before-completion**: 在宣称工作完成、已修复或测试通过之前使用，在提交或创建 PR 之前——必须运行验证命令并确认输出后才能声称成功；始终用证据支撑断言
- **workflow-runner**: 在 Claude Code / OpenClaw / Cursor 中直接运行 agency-orchestrator YAML 工作流——无需 API key，使用当前会话的 LLM 作为执行引擎。当用户提供 .yaml 工作流文件或要求多角色协作完成任务时触发。
- **writing-plans**: 当你有规格说明或需求用于多步骤任务时使用，在动手写代码之前
- **writing-skills**: 当创建新技能、编辑现有技能或在部署前验证技能是否有效时使用

## 如何使用

当任务匹配某个 skill 时，使用 `Skill` 工具加载对应 skill 并严格遵循其流程。绝不要用 Read 工具读取 SKILL.md 文件。

如果你认为哪怕只有 1% 的可能性某个 skill 适用于你正在做的事情，你必须调用该 skill 检查。
<!-- superpowers-zh:end -->
