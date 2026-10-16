# 工具模块（util）与异常（exception）

## 功能概述

工具模块提供框架通用的辅助函数：程序运行时间（`TimeUtil`）、字符串处理（`StringUtil`）、系统能力（`SystemUtil`，含调用栈、雪花算法、CPU 核心数），以及框架统一异常体系（`bus::Exception` 命名空间）。`Hook`（系统调用 Hook）为规划中的协程基础设施，当前为空实现。

## 使用示例

```cpp
#include "util/Util.h"
#include "exception/Exception.hpp"

using namespace bus;

// 程序运行时长（基于自定义 epoch 2025-01-01）
Timestamp elapsed = TimeUtil::getElapse();

// 文件名（日志宏内部使用）
const char* base = StringUtil::BaseFileName(__FILE__);

// 文件大小字符串解析："5MB" -> 字节数
off64_t size = StringUtil::StrToFileSize("5MB");

// 调用栈
std::string trace = SystemUtil::BacktraceToString(32, 0);

// CPU 核心数
uint32_t cores = SystemUtil::getCoreNum();

// 异常
try {
    throw Exception::ConfigError("parse failed");
} catch (const Exception::ConfigError& e) {
    std::cout << e.what() << std::endl;
}
```

## 接口说明

### TimeUtil

| 接口 | 说明 |
|---|---|
| `getElapse()` | 程序运行时间（自启动起） |
| `getEpoch()` | 业务自定义 epoch：**2025-01-01** 的时间戳（`constexpr`） |

### StringUtil

| 接口 | 说明 |
|---|---|
| `BaseFileName(filename)` | 编译期提取文件名（去掉目录前缀，日志宏使用） |
| `StrToFileSize(str)` | 解析文件大小字符串（如 `"4MB"`、`"5MB"`）为字节数 |

### SystemUtil

| 接口 | 说明 |
|---|---|
| `Backtrace(size, skip)` | 获取调用栈信息（返回符号列表） |
| `BacktraceToString(size, skip, prefix)` | 调用栈格式化字符串 |
| `Abort()` | 终止进程（配合 `ASSERT`） |
| `getWorkId()` | 机器 ID（当前恒返回 1，待完成） |
| `generateSnowflakeKey()` | 雪花算法生成 key（基于自定义 epoch） |
| `getCoreNum()` | CPU 核心数量 |

### 异常体系（`bus::Exception` 命名空间）

| 异常 | 基类 | 触发场景 |
|---|---|---|
| `ConfigUnLoad` | `std::exception` | 配置未加载时访问 |
| `ConfigError` | `std::logic_error` | TOML 解析/加载失败 |
| `LoggerError` | `std::logic_error` | 日志模块错误 |
| `DateTimeFormatError` | `std::logic_error` | 日期时间格式非法 |
| `AddressError` | `std::logic_error` | 地址解析错误 |
| `SocketError` | `std::logic_error` | Socket 操作错误 |

### Hook（规划中）

`src/Hook.h` 当前仅定义了空的 `bus::Hook` 命名空间。规划用于协程模块对系统调用（`read`/`write`/`sleep` 等）的 Hook，使阻塞调用在协程框架下让出执行权，**尚未实现**。

## 注意事项

- `TimeUtil::getEpoch()` 为业务自定义 epoch（2025-01-01），与 Unix epoch（1970-01-01）不同；雪花算法生成 key 的可用性依赖于此约定，跨系统对接时注意换算
- `SystemUtil::getWorkId()` 为占位实现（恒为 1），分布式部署前需完成机器 ID 分配方案
- `Backtrace` 系列接口依赖符号表，Release 构建（`-O2` 无 `-g`）下可能只有地址而无函数名
- `Hook.h` 为空实现，请勿在当前版本中使用
