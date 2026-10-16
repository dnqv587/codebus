# 日志模块（log）

## 功能概述

日志模块提供六级日志、格式化模板、多 Logger 管理、文件滚动（同步/异步落盘）与标准输出 Appender，支持通过 TOML 配置文件定义 Logger 行为。

## 设计原理

```
LoggerManager（单例，管理 Logger 集合）
   └── Logger（名称、级别、格式化器、Appender 集合）
          ├── LogFormatter（%d %m %p ... 模板解析 → FormatItem）
          └── LogAppender（输出目标）
                 ├── FileLogAppender（SYNC: SyncAppendFile / ASYNC: AsynAppendFile）
                 └── StdoutLogAppender
```

- **调用路径**：日志宏 `LOG_INFO(logger) << ...` 构造临时 `Logging` 对象，收集 `LogContext`（文件/行号/函数/线程/协程/时间等元信息），析构时经 `LogFormatter` 格式化后分发给各 `Appender`
- **异步落盘**：`AsynAppendFile` 使用后台线程 + `Semaphore` + 队列实现生产者-消费者模型，调用线程不阻塞于磁盘 IO
- **文件滚动**：`AppendFile::RollFile()` 按单文件大小（默认 4MB）滚动，文件名带时间戳后缀
- **配置驱动**：`LoggerManager::LoadConfig()` 从系统配置（`sys_config`）的 `logger` 数组读取 Logger 定义

## 使用示例

### 配置文件（sys.toml）

```toml
[[logger]]
name = "sys"
path = "log"
level = "DEBUG"
EnableStdout = true
format = "%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n"
action = "ASYNC"
FlushInterval = 3
FlushLogCount = 1024
SingleFileSize = "5MB"
```

### 代码

```cpp
#include "log/Logger.h"
#include "Config.h"

using namespace bus;

static Logger& g_logger = GET_LOGGER("sys");

int main() {
    // 系统配置 sys_config 启动时自动加载 ./sys.toml，
    // 此后按配置初始化 logger
    LoggerManager::getInstance()->LoadConfig("sys");

    LOG_TRACE(g_logger) << "trace message";
    LOG_INFO(g_logger) << "info message: " << 42;
    LOG_FMT_WARN(g_logger, "formatted: %s %d", "answer", 42);
    return 0;
}
```

## 接口说明

### 日志级别（`LogLevel`）

`TRACE` < `DEBUG` < `INFO` < `WARN` < `ERROR` < `FATAL`（仅输出 ≥ Logger 当前级别的日志）。

### 日志宏

| 宏 | 说明 |
|---|---|
| `LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL(logger)` | 流式日志 |
| `LOG_FMT_TRACE/.../FATAL(logger, fmt, ...)` | printf 风格日志 |
| `GET_LOGGER(name)` | 获取（或创建）指定名称的 Logger |
| `LOG_THROW(ex)` | 记录 FATAL 日志并返回异常对象（配合 `throw`） |
| `ASSERT_THROW(expr, ex)` | 表达式为假时记录日志并抛出异常 |
| `CHECK_NOTNULL(val)` | 检查指针非空，为空时记录 FATAL 日志 |

> 注意：旧版 `LOG_ROOT` 宏已移除（`LoggerManager::getRoot()` 暂未实现）。

### Logger

| 接口 | 说明 |
|---|---|
| `Logger(name, level)` | 构造，默认级别 DEBUG |
| `getLevel()` / `setLevel()` | 获取/设置级别 |
| `setFormatter(pattern)` | 按模板字符串设置格式化器 |
| `addAppender(ptr)` / `delAppender(ptr)` | 增删输出目标 |
| `getName()` | Logger 名称 |

### LoggerManager（单例）

| 接口 | 说明 |
|---|---|
| `getInstance()` | 获取单例 |
| `getLogger(name)` | 获取指定 Logger（不存在时创建） |
| `LoadConfig(name)` | 从 `sys_config` 的 `logger` 数组加载配置（`noexcept`，配置缺失时静默返回） |

### 日志格式模板（`LogFormatter`）

| 占位符 | 含义 | 占位符 | 含义 |
|---|---|---|---|
| `%m` | 消息内容 | `%p` | 日志级别 |
| `%r` | 程序启动累计毫秒数 | `%c` | 日志名称 |
| `%t` | 线程 ID | `%n` | 换行 |
| `%d{fmt}` | 时间（strftime 格式） | `%f` | 文件名 |
| `%l` | 行号 | `%M` | 函数名 |
| `%T` | 制表符 | `%F` | 协程 ID |
| `%N` | 线程名 | `%h` | 主机名 |
| `%P` | 进程号 | `%%` | 字面 `%` |

默认模板：`%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n`

### LogAppender

| 类 | 说明 |
|---|---|
| `FileLogAppender(logName, logPath, action, flushInterval, flushLogCount, singleFileSize)` | 文件输出；`action` 取 `AppenderAction::SYNC` 或 `ASYNC`；`singleFileSize` 支持 `_MB` 字面量 |
| `StdoutLogAppender()` | 标准输出 |

## 注意事项

- `LoggerManager::getLogger()` 对配置文件中未定义但代码中获取的 Logger 的处理逻辑待完善（源码中有 TODO 标注）
- `LogContext` 中的协程 ID（`%F`）当前恒为 0（协程模块尚未实现）
- 异步 Appender 销毁前请确保调用 `stop()`（由 `AsynAppendFile` 析构处理），避免后台线程泄漏
- 格式化器线程安全：`Logger` 内部使用 `RWLock` 保护格式化器与 Appender 集合
