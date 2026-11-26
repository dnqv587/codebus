# 项目名称
codebus
# 项目概述
- 项目是一个基于C++20的高性能服务器框架库，包含以下几个模块：
  - 网络模块：实现网络通信的功能，包括TCP、UDP、HTTP（Servlet/Filter 容器模型）等协议。
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
- 你生成的所有文档都需要符合github风格的形式来书写。
- 你所有生成的代码都需要符合C++20标准，代码风格统一，符合现代C++最佳实践。
- 当任务完成时，你需要告诉用户任务完成的情况并附证据和文件链接。
- 当你需要向用户提供多个选择项时，必须详细描述问题，并且告诉用户不同选择的优缺点，以及改动代码的前后差异和代码的位置

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

测试位于 `test/` 目录，使用 gtest（`3rd/gtest/`，链接 `gtest_main`）并接入 CTest。**一单元一测试目标**（目录镜像 src/ 结构，共 64 个目标）：

- 根级（镜像 src 根级源文件）：`SliceTest`、`BufferTest`、`ConfigTest`、`TimerTest`、`ExceptionTest`、`TypeTest`、`MacroTest`、`ViewTest`、`SingletonTest`、`BlockingQueueTest`、`TimestampTest`、`DateTimeTest`、`TimeUtilTest`、`StringUtilTest`、`SystemUtilTest`
- `log/`：`LogFormatterTest`、`LogAppenderTest`、`LoggingTest`、`LoggerTest`、`LoggerManagerTest`
- `thread/`：`MutexTest`、`ConditionTest`、`SemaphoreTest`、`CountDownLatchTest`、`ThreadTest`、`ThreadPoolTest`、`ProcessInfoTest`
- `net/`：`AddressTest`、`SocketTest`、`ChannelTest`、`EventDispatcherTest`、`EventLoopTest`、`EventLoopThreadTest`、`EventLoopThreadPoolTest`、`ListenerTest`、`TCPConnectionTest`、`TcpServerTest`、`ConnectorTest`、`TcpClientTest`、`TlsConnectionTest`、`UdpTest`、`UdpClientTest`
- `net/ssl/`：`SslContextTest`、`SslSessionTest`
- `net/http/`：`HttpProtocolTest`、`HttpUtilTest`、`HttpMessageTest`、`HttpRequestParserTest`、`ServletDispatcherTest`、`HttpServerTest`、`HttpServerWorkerTest`、`HttpClientTest`、`HttpConnectionPoolTest`、`SessionTest`、`StaticFileServletTest`、`HttpCompressionTest`、`HttpStreamingTest`、`HttpsServerTest`、`HttpsClientTest`
- `net/websocket/`：`WsFrameTest`、`WebSocketServerTest`、`WssServerTest`、`WsClientTest`
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
  - zlib（系统库，gzip——`ZLIB::ZLIB`，H3 接入）
  - OpenSSL（系统库，TLS——`OpenSSL::SSL`/`OpenSSL::Crypto`，H4 接入；测试证书资产见 `test/certs/`）
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
- 支持 Clang 线程安全注解（需 `-Wthread-safety` 编译选项，见 `src/Macro.h`）

## 资源管理与所有权约定

- 全部使用 RAII 模式（锁、fd、智能指针）
- 所有权语义三种：
  - 借用裸指针：不拥有生命周期，装配点保证非空（如 `Channel`、`Listener`、`Connector`、`TimerManager` 持有所属 `EventLoop*`；与 muduo 一致）
  - `std::shared_ptr`：共享所有权
  - `std::unique_ptr`：独占所有权
- 使用借用指针时须确保被引用对象生命周期长于持有者本身（`View<T>`（`src/view.h`）保留但生产代码已不再使用）
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

## 3. 单例模式
- 管理器类（如 `LoggerManager`）使用 `Singleton<T>` 模板
- 线程安全的懒加载初始化

## 4. 缓冲区设计
- `Slice`（`Buffer.hpp`）：基于 `std::span<std::byte>` 的字节视图
- `Buffer`：预留/可读/可写三区结构，滑动窗口增长
- 文件大小字面量：`4_MB`、`128_MB`（定义于 `src/type.h`）

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

项目目标中提及但尚未实现的模块（gzip、WebSocket 帧层、通用流式、HTTPS/TLS/wss 已随 H3/H4 落地移出）：
- HTTP2/H3（远期）
- 异步 Servlet、multipart、限流（H5 治理轮：与连接池/重定向/XFF/WS 客户端/ETag 完整同轮）
- 内存模块（内存池、内存分配）
- Lua 模块（脚本执行）
- 数据库和缓存模块
- `Hook`（系统调用 Hook，协程基础设施）为空实现

# 目录结构
 - `3rd/` - 第三方库（tomlc17、gtest、http_parser）
 - `bin/` - 可执行程序生成位置
 - `build/` - CMake生成脚本位置
 - `docs/` - 模块文档（`docs/README.md` 为索引），`docs/spec/` 为设计文档与计划
 - `lib/` - 动态库生成位置
 - `src/` - 源代码（根级：全局工具类与基础设施——宏/基类/单例/View/阻塞队列、
   时间（Timestamp、DateTime）、工具（Util）、缓冲区、配置、异常、Hook、定时器）
  - `src/log/` - 日志
  - `src/net/` - 网络（`src/net/ssl/` TLS、`src/net/http/` HTTP 协议族、`src/net/websocket/` WebSocket）
  - `src/thread/` - 线程
 - `test/` - 测试

# 参考资料

本项目设计参考：
- [muduo](https://github.com/chenshuo/muduo) - 高性能 C++ 网络库
- [sylar](https://github.com/sylar-yin/sylar) - 高性能服务器框架

## Agent skills

### Issue tracker

Issues 以本地 markdown 文件跟踪于 `docs/scratch/<feature>/`（不使用 GitHub Issues）。见 `docs/agents/issue-tracker.md`。

### Triage labels

使用五个默认 canonical role 同名 label（needs-triage / needs-info / ready-for-agent / ready-for-human / wontfix）。见 `docs/agents/triage-labels.md`。

### Domain docs

Single-context：根目录 `GLOSSARY.md` + `docs/adr/`（懒创建）。见 `docs/agents/domain.md`。
