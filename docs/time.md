# 时间模块（time）

## 功能概述

时间模块提供两类时间抽象：

- **`Timestamp`**：微秒精度 UTC 时间戳，支持格式化输出、算术运算与比较
- **`Date` / `Time` / `TimeZoneOffset` / `DateTime`**：RFC 3339 风格的日历日期时间组件

## 设计原理

- `Timestamp` 内部存储自 Unix epoch 起的微秒数（`int64_t`），提供 `toTimespec()` / `toTimeval()` 与 POSIX API 互操作
- `Timestamp` 继承 `less_than_comparable<Timestamp>`：实现 `operator<` 后自动获得 `>`、`>=`、`<=`
- `DateTime` 由 `Date`（年月日）+ `Time`（时分秒纳秒）+ `TimeZoneOffset`（UTC 偏移）组合，构造函数即校验合法性，非法值通过 `ASSERT_THROW` 抛出 `Exception::DateTimeFormatError`

## 使用示例

```cpp
#include "time/Timestamp.h"
#include "time/DateTime.h"
#include <iostream>

using namespace bus;

// 时间戳
Timestamp now = Timestamp::now();
std::cout << now.toFormatString("%Y-%m-%d %H:%M:%S") << std::endl;

Timestamp tomorrow = now.nextDay();
Timestamp today0 = now.toDay();
auto diff = tomorrow - now;          // 支持 + - * / % 运算
auto sec  = now.getSecond();
auto ms   = now.getMilliSecond();
auto us   = now.getMicroSecond();

// 字面量
Timestamp t1 = 123456789_stamp;

// RFC 3339 日期时间
Date d(2025, 1, 19);
Time t(14, 30, 0, 123000000);              // 14:30:00.123
TimeZoneOffset offset(8, 0);               // UTC+8
DateTime dt(d, t, offset);
std::cout << dt << std::endl;              // 2025-01-19T14:30:00.123+08:00
```

## 接口说明

### Timestamp

| 接口 | 说明 |
|---|---|
| `Timestamp(us)` | 从微秒数构造 |
| `now()` | 当前时间戳 |
| `toFormatString(fmt, isLocal)` | 按 strftime 格式输出；`isLocal` 控制本地时区/UTC |
| `nextDay()` | 下一日 0 点（UTC） |
| `toDay()` | 当日起始（UTC） |
| `getMicroSecond()` / `getMilliSecond()` / `getSecond()` | 微秒/毫秒/秒 |
| `toTimespec()` / `toTimeval()` | 转 POSIX 结构 |
| `operator+ - * / %` | 时间戳算术 |
| `operator< <= > >= ==` | 比较（`less_than_comparable`） |
| `operator<<` | 输出微秒数值 |

常量：`CONS_MicroSecondPerSecond`（每秒微秒数）、`CONS_MicroSecondPerDay`（每天微秒数）。

### Date / Time / TimeZoneOffset / DateTime

| 类型 | 字段/接口 | 说明 |
|---|---|---|
| `Date` | `year` `month` `day`；`isLeapYear()` | 构造校验月份 1-12、日数不超过当月最大值 |
| `Time` | `hour` `minute` `second` `nanoSecond` | 构造校验小时 <24、分/秒 <60、纳秒 <10^9 |
| `TimeZoneOffset` | `toString()` | 构造校验范围 ±12h；`DateTime::useSpaceSign()` / `revertSign()` 切换输出分隔符（空格/`T`） |
| `DateTime` | `date` `time` `offset` | 组合类型，支持流输出 |

## 注意事项

- `Timestamp` 为 **UTC** 时间；`toFormatString` 默认 `isLocal = true` 转本地时区输出
- `Timestamp::getMicroSecond()` 返回 `time_t`（秒级宽度），微秒值超 `time_t` 范围时存在截断风险，仅供当前时间范围使用
- `DateTime` 构造校验依赖 `ASSERT_THROW`（记录 FATAL 日志并抛出），非法日期会抛出 `Exception::DateTimeFormatError`
- 工具模块 `TimeUtil::getEpoch()` 定义的业务自定义 epoch 为 **2025-01-01**（用于雪花算法等场景），与 Unix epoch 不同，见 [util.md](util.md)
