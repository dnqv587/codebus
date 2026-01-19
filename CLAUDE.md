# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 编译和测试

### 编译命令

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
- 库文件：`../lib/`（静态库和动态库）
- 可执行文件：`../bin/`

### 运行测试

独立测试程序位于 `test/` 目录：

```bash
# 从 build 目录执行
../bin/EventLoopTest
../bin/LoggerTest
../bin/ThreadTest
../bin/ThreadPoolTest
../bin/ConfigTest
../bin/TimerTest
```

### 依赖项

- **编译器：** GCC 7+ 或 Clang 5+（需 C++17 支持）
- **CMake：** 3.0+
- **操作系统：** Linux（POSIX 系统）
- **库：**
  - `pthread`（自动链接）
  - toml++（位于 `3rd/toml/`）
  - gtest（位于 `3rd/gtest/` - 当前未使用）

## 核心架构设计

### 事件驱动架构（One Loop Per Thread）

- 每个 IO 线程拥有独立的 `EventLoop`
- `Channel` 封装文件描述符和事件
- `EventDispatcher`（基于 epoll）分发 I/O 事件
- 使用 `RunInLoop()` 从其他线程安全地执行任务

**关键调用链：**
```
EventLoop::Loop()
  -> EventDispatcher::wait() (epoll_wait)
  -> Channel::HandleEvent() 分发回调
  -> 执行 PendingFunctors (跨线程任务)
```

### 线程安全机制

- 所有不可复制类继承 `noncopyable` 基类
- 互斥锁使用：`MutexLock` + `MutexLockGuard`
- 读写锁使用：`RWLock` + `ReadLockGuard`/`WriteLockGuard`
- 支持 Clang 线程安全注解（需 `-Wthread-safety` 编译选项）

### 资源管理

- 全部使用 RAII 模式（锁、智能指针）
- 智能指针约定：`std::shared_ptr` 用于共享所有权，`std::unique_ptr` 用于独占所有权
- 字符串参数优先使用 `std::string_view` 而非 `const std::string&`

## 重要架构要点

### 1. Channel 生命周期
- 每个 `Channel` 只属于一个 `EventLoop`（IO 线程）
- 永远不要跨线程共享 Channel
- 使用 `OwnerLoop()` 检查所属的 EventLoop

### 2. IO 线程安全
- 使用 `EventLoop::isInLoopThread()` 检查是否在所属 IO 线程
- 使用 `RunInLoop(callback)` 安全地排队执行跨线程任务
- `Wakeup()` 通过 eventfd 唤醒阻塞的 epoll_wait

### 3. ServerStatus 模板
- 需要启动/停止状态的类继承 `ServerStatus<bool>`
- 示例：`class EventLoop: ServerStatus<true>, noncopyable`

### 4. 单例模式
- 管理器类（如 `LoggerManager`）使用 `Singleton<T>` 模板
- 线程安全的懒加载初始化

### 5. 类型别名约定
- 统一使用 `using ptr = std::shared_ptr<T>` 定义指针类型
- 详见各模块头文件

## 模块交互关系

### 网络模块交互
```
TcpServer -> EventLoop -> EventDispatcher(epoll)
                           -> Channel(fd) -> 回调函数
```

### 配置系统
```
Config4Toml -> TOML 解析器 -> Value(抽象基类)
                             -> Value4Toml(具体实现)
```

### 日志系统
```
Logger -> LogFormatter -> LogAppender(控制台/文件) -> 输出
```

## 关键实现细节

### EventLoop 线程模型
- `EventLoop::Loop()` 在所属线程的 `epoll_wait()` 中阻塞
- `RunInLoop(callback)` 将任务排队到 `m_PendingFunctor`
- `Wakeup()` 通过 `m_WakeupChannel` (eventfd) 中断 epoll_wait
- `doPendingFunctor()` 在每次循环开始时执行跨线程任务

### Channel 事件处理
- 通过 `EnableReadEvent()`、`EnableWriteEvent()` 注册事件
- 使用 `setReadCallback()`、`setWriteCallback()`、`setErrorCallback()` 设置回调
- `HandleEvent()` 根据 `m_OccurEvents` 分发到相应回调

### Thread 类
- 底层使用 `pthread`
- 继承自 `CurrentThread` 获取线程本地数据
- 支持通过 `bindCPU()` 绑定 CPU 核心亲和性
- 使用 `CountDownLatch` 确保线程完全启动后 `run()` 才返回

### 配置系统注意事项
- 当前主实现为 `Config4Toml`（TOML 格式）
- `ConfigManager` 类在代码中已被注释掉（未激活）
- 配置访问通过 `GET_ROOT_CONFIG("key")` 宏

## 代码风格要求

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
LOG_ROOT              // 获取 root logger
GET_LOGGER("name")     // 获取命名 logger
```

### 断言宏
```cpp
ASSERT(expr, msg)              // 运行时断言
ASSERT_THROW(expr, exception)  // 断言或抛出异常
CHECK_NOTNULL(ptr)             // 检查指针非空
```

## 待实现模块

项目目标中提及但尚未实现的模块：
- 内存模块（内存池、内存分配）
- Lua 模块（脚本执行）
- UDP 协议支持（网络模块）
- 数据库和缓存模块

## 参考资料

本项目设计参考：
- [muduo](https://github.com/chenshuo/muduo) - 高性能 C++ 网络库
- [sylar](https://github.com/sylar-yin/sylar) - 高性能服务器框架
