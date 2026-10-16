# Codebus 文档

本目录包含 codebus 框架各模块的功能说明、设计原理、使用示例与接口文档。

> ⚠️ 文档基于当前工作区的目标 API（重构方向）编写，部分接口处于重构中，实际行为以源码为准；文档中已标注已知问题与未完成项。

## 模块导航

| 模块 | 文档 | 关键组件 |
|---|---|---|
| 基础组件 | [base.md](base.md) | 宏（`Macro.h`）、`noncopyable`/`copyable`、`less_than_comparable`、`Singleton`、`View<T>`、`BlockingQueue`、`Observer`、`type.h` |
| 缓冲区 | [buffer.md](buffer.md) | `Slice`、`Buffer` |
| 日志 | [log.md](log.md) | `Logger`、`LoggerManager`、`LogFormatter`、`LogAppender`（文件/标准输出）、日志宏 |
| 网络 | [net.md](net.md) | `EventLoop`、`Channel`、`EventDispatcher`、`Address`、`Socket`、`Listener`、`TCPConnection`、`TcpServer`、`ServerStatus` |
| 线程 | [thread.md](thread.md) | `Thread`、`ThreadPool`、`MutexLock`、`RWLock`、`Condition`、`Semaphore`、`CountDownLatch` |
| 时间 | [time.md](time.md) | `Timestamp`、`Date`、`Time`、`TimeZoneOffset`、`DateTime` |
| 定时器 | [timer.md](timer.md) | `TimerManager`、`Timer` |
| 配置 | [config.md](config.md) | `Config`、`Value`、`SYS_CONF`、`sys_config` |
| 工具 | [util.md](util.md) | `TimeUtil`、`StringUtil`、`SystemUtil`、`Exception`、`Hook` |

## 快速上手

1. 阅读 [README.md](../README.md) 了解项目定位与编译方式
2. 按需查阅各模块文档的"使用示例"章节
3. 每个模块文档均包含：功能概述、设计原理、使用示例、接口说明、注意事项

## 术语约定

- **IO 线程**：持有 `EventLoop` 并运行 `Loop()` 的线程，每个 IO 线程只运行一个 `EventLoop`
- **`View<T>`**：所有权视图，不拥有对象生命周期，等价于受控引用（见 [base.md](base.md)）
- **`ptr` 别名**：各类型内部使用 `using ptr = std::shared_ptr<T>` 定义共享指针类型
- **RAII**：所有资源（锁、文件描述符、文件句柄）均通过 RAII 管理，类不可复制时继承 `noncopyable`
