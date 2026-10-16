# 配置模块（Config）

## 功能概述

配置模块基于 [tomlc17](https://github.com/cktan/tomlc17)（C 语言 TOML 解析器）提供 TOML 配置加载与类型安全访问：`Config` 负责加载解析，`Value` 抽象配置值并提供 `std::optional` 风格的类型安全转换，支持点号分隔的路径查询（如 `"server.port"`）。系统级配置通过全局 `sys_config` 对象与 `SYS_CONF` 宏访问。

## 设计原理

```
Config（加载、解析、线程安全访问）
  └── Handle（tomlc17 解析句柄的 RAII 封装）
        └── tomlc17 解析树
Value（配置值抽象基类，类型检查 + optional 转换）
  ├── Value4Toml（具体值：字符串/整数/浮点/布尔/日期/时间/数组/表/数组表）
  └── NullValue（单例，键不存在时返回）
```

- **加载**：`LoadFile(path)` / `Load(doc)` 通过 tomlc17 解析，失败抛出 `Exception::ConfigError`；未加载时访问抛出 `Exception::ConfigUnLoad`
- **取值**：`get(name)` 按点号路径查询，返回 `Value::ptr`；键不存在返回 `NullValue` 单例（其所有 `isXxx()` 为 false、`asXxx()` 为 `nullopt`）
- **类型安全转换**：`asString()` / `asInteger()` 等返回 `std::optional`，类型不匹配返回 `nullopt`，不会静默转换出错
- **线程安全**：`Config` 内部使用 `std::shared_mutex`，读多写少场景友好
- **系统配置**：`inline Config sys_config` 全局对象在静态初始化期自动加载 `./sys.toml`（存在时），配合 `SYS_CONF(XX)` 宏（展开为 `sys_config.get(XX)`）访问

## 使用示例

```cpp
#include "Config.h"
#include <iostream>

using namespace bus;

// 1. 系统配置（自动加载 ./sys.toml）
auto port = SYS_CONF("server.port");
if (auto v = port->asInteger()) {
    std::cout << "server.port = " << *v << std::endl;
}

// 2. 独立配置对象
Config conf;
conf.LoadFile("conf/app.toml");

// 字符串
std::cout << conf.get("string_test.name")->asString().value_or("") << std::endl;

// 数组
auto arr = conf.get("array_test.names")->asArray().value();
for (auto& item : arr) {
    std::cout << "arr: " << item->asString().value() << std::endl;
}

// 表
auto tab = conf.get("float_test")->asTable().value();
for (auto& [key, val] : tab) {
    std::cout << "table: " << key << ":" << val->asFloat().value() << std::endl;
}

// 点号路径（嵌套）
std::cout << conf.get("nested_test.server.host")->asString().value() << std::endl;

// 键不存在：返回 NullValue 单例，所有取值返回 nullopt
auto miss = conf.get("not.exist");
std::cout << "isString: " << miss->isString() << std::endl;   // 0
```

## 接口说明

### Config

| 接口 | 说明 |
|---|---|
| `Config(type = ConfigType::TOML)` | 构造（当前仅支持 TOML） |
| `LoadFile(path)` | 从文件加载；失败抛 `Exception::ConfigError` |
| `Load(doc)` | 从字符串加载；失败抛 `Exception::ConfigError` |
| `get(name)` | 按点号路径查询，返回 `Value::ptr`；未加载抛 `Exception::ConfigUnLoad`；键不存在返回 `NullValue` 单例 |
| `isLoaded()` | 是否已加载 |

### Value（抽象基类）

| 类型检查 | 类型转换（返回 `std::optional`） |
|---|---|
| `isString()` | `asString()` → `std::optional<std::string_view>` |
| `isInteger()` | `asInteger()` → `std::optional<int64_t>` |
| `isFloat()` | `asFloat()` → `std::optional<double>` |
| `isBoolean()` | `asBoolean()` → `std::optional<bool>` |
| `isDate()` | `asDate()` → `std::optional<year_month_day>` |
| `isTime()` | `asTime()` → `std::optional<hh_mm_ss<us>>` |
| `isDateTime()` | `asDateTime()` → `std::optional<sys_time<us>>` |
| `isArray()` | `asArray()` → `std::optional<vector<Value::ptr>>` |
| `isTable()` | `asTable()` → `std::optional<unordered_map<string_view, Value::ptr>>` |
| — | `asArrayTable()` → `std::optional<vector<table_t>>`（`[[items]]` 数组表） |
| — | `seek(name)` → 子键查询（表内取值） |

`Value` 支持 `operator<<` 直接流输出。

### 宏与全局对象

| 名称 | 说明 |
|---|---|
| `SYS_CONF(XX)` | `sys_config.get(XX)` 的简写，访问系统配置 |
| `sys_config` | 全局系统配置对象，启动时自动加载 `./sys.toml`（不存在则跳过） |

## 注意事项

- **与旧版 API 的区别**：旧版 `Config4Toml` / `ConfigManager` / `GET_ROOT_CONFIG` 已删除，本版为 `Config` + `Value` + `SYS_CONF`，无配置值缓存（每次 `get` 直接查询解析树）
- `asString()` 返回 `string_view`，指向解析树内部内存，其生命周期随 `Config` 对象；`Config` 重新 `LoadFile` 后旧 `string_view` 失效
- `sys_config` 加载路径 `./sys.toml` 为**相对当前工作目录**，服务部署时请确保工作目录正确
- 异常层次：`ConfigUnLoad`（未加载访问）、`ConfigError`（解析失败），均定义于 [util.md](util.md) 所述 `bus::Exception` 命名空间
- `Config` 的 `get()` 返回 `Value::ptr`（shared_ptr），可安全地跨过 `Config` 生命周期保存（但其中的 `string_view` 不保证）
