# 项目名称
codebus
# 项目概述
- 项目是一个基于C++20的网络框架库，包含以下几个模块：
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

测试程序位于 `test/` 目录，由 `test/CMakeLists.txt` 管理（gtest 位于 `3rd/gtest/`）。测试目标有：`LoggerTest`、`ConfigTestNew`、`NetTest`、`EventLoopTest`、`ThreadTest`、`ThreadPoolTest`、`TimerTest`、`TomlTest`、`DateTimeTest`、`UtilTest`、`logFormatterTest` 等。

```bash
# 从 build 目录执行
../bin/LoggerTest
../bin/ConfigTestNew
../bin/NetTest
```

注意：重构期间部分测试在 CMakeLists 中被注释，重构收尾后需全部重新启用并通过。

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
TcpServer -> EventLoop -> EventDispatcher(epoll)
                           -> Channel(fd) -> 回调函数
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

**所有代码必须遵循 [`.claude/rules/code_style.md`](.claude/rules/code_style.md)：**

### 必需的文件头注释
```cpp
//***********************************************
// name      : 文件名
// brief     : 文件功能简要描述
// attention : 注意事项，如无则写 None
//***********************************************
```

### 命名规范
- 类名：`PascalCase`
- 函数名：`PascalCase`
- 成员变量：`m_PascalCase`（需 `/// @brief` 注释）
- 命名空间：`bus`

### 属性宏使用（来自 `base/Macro.h`）

| 宏 | 用途 |
|---|---|
| `ATTR_INLINE` | 强制内联 |
| `ATTR_NODISCARD` | 必须检查返回值 |
| `ATTR_PURE` | 纯函数（返回值仅依赖参数） |
| `ATTR_CONST` | 纯函数（不依赖全局状态） |
| `ATTR_PURE_INLINE` | 内联纯 getter |
| `LIKELY(x)` / `UNLIKELY(x)` | 分支预测提示 |

### 日志宏（来自 `log/Logger.h`）
```cpp
LOG_INFO(logger) << "message";
LOG_FMT_INFO(logger, "format %d", value);
GET_LOGGER("name")     // 获取命名 logger
```

### 断言宏
```cpp
ASSERT(expr, msg)              // 运行时断言
ASSERT_THROW(expr, exception)  // 断言或抛出异常
CHECK_NOTNULL(ptr)             // 检查指针非空
```

# 待实现模块

项目目标中提及但尚未实现的模块：
- 内存模块（内存池、内存分配）
- Lua 模块（脚本执行）
- UDP 协议支持（网络模块，`Socket::Type` 已预留枚举）
- 数据库和缓存模块
- 网络模块收尾：`TcpServer` 实现未完成、`EventLoopThreadPool` 为空壳
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
