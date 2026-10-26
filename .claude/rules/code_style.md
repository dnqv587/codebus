# Codebus C++ 代码风格规范

本项目基于 **C++20**。本规范面向**新代码**：存量旧代码存在 TAB 缩进、命名混杂等问题，将逐步收敛，生成代码**勿模仿**旧代码。

## 1. 文件头注释（每个源文件必须）

```cpp
//***********************************************
// name      : <文件名>
// brief     : <功能简要描述>
// attention : <注意事项，如果没有则写 None>
//***********************************************
```

## 2. 命名规范

| 元素 | 规则 | 示例 |
|---|---|---|
| 命名空间 | 统一小写 `bus` | `namespace bus` |
| 类名 | PascalCase，描述性 | `class EventLoop` |
| 函数 | 按语义分三层（见下） | — |
| 成员变量 | `m_` 前缀 + PascalCase，须带 `/// @brief` 注释 | `pid_t m_Tid;` |
| 常量 | `k` 前缀 PascalCase 或全大写下划线分隔 | `kMaxSize` / `MAX_SIZE` |
| 宏 | 全大写下划线分隔 | `LOG_INFO` |
| 类型别名 | `using`（禁用 typedef），PascalCase | `using FunctorList = std::vector<Functor>;` |

**函数命名三层分规**（与现有代码一致）：

- **getter / setter / 判断**：camelCase —— `getName()`、`setReadCallback()`、`isInLoopThread()`
- **动作方法**：PascalCase —— `RunInLoop()`、`HandleEvent()`、`Write()`
- **STL 语义的值类型方法**：全小写 —— `size()`、`empty()`、`begin()`

智能指针别名模式（框架核心类常用）：`using ptr = std::shared_ptr<X>;`，变体有 `ref`、`unique_ptr`、`weak_ptr`。

## 3. 注释规范（公共接口必须，Doxygen 风格）

```cpp
/// @brief <函数功能描述>
/// @param <参数名> <参数说明>
/// @return <返回值说明>
/// @throw <异常类型> <异常说明（如有）>
/// @attention <注意事项（如有）>
```

**示例：**

```cpp
/// @brief 将线程绑定到 CPU 固定核心
/// @param cpuid 核心号，必须小于系统核心总数
/// @throw std::runtime_error 绑定失败时抛出
/// @attention 仅 Linux 平台有效
void bindCPU(uint32_t cpuid);
```

## 4. 代码格式

- 缩进 **4 个空格**，禁止 TAB（存量旧代码含 TAB，勿模仿）
- 类定义左大括号**换行**；多行函数体 `)` 后**换行** `{`；单行函数体**内联** `{ ... }`
- 访问修饰符缩进一空格：` public:`
- 指针 / 引用符号贴类型：`Channel* channel`、`const std::string& name`
- include 顺序：对应头文件 → 项目头文件（相对路径）→ 标准库 → 第三方

```cpp
class Thread : noncopyable
{
 public:
    void Run();                      // 动作方法：PascalCase
    std::string getName() const;     // getter：camelCase
 private:
    /// @brief 线程 id
    pid_t m_Tid;
};
```

## 5. C++ 特性

- 头文件保护用 `#pragma once`，不用 `#ifndef`
- 虚函数重写标 `override`，禁止进一步继承标 `final`
- 不抛异常的函数标 `noexcept`；析构函数一律 `noexcept`
- 构造 / 传参用 `std::move` / `std::forward` 转移所有权，避免拷贝
- 复杂迭代器等声明用 `auto`；空指针用 `nullptr`
- 所有权语义：共享 `std::shared_ptr`、独占 `std::unique_ptr`、打破循环 `std::weak_ptr`
- 字符串参数优先 `std::string_view` 而非 `const std::string&`；大对象按引用或右值传递

## 6. 项目宏（`base/Macro.h`、`log/Logger.h`）

| 宏 | 用途 |
|---|---|
| `ATTR_NODISCARD` | 必须检查返回值 |
| `ATTR_PURE_INLINE` | 内联纯 getter（最常用） |
| `ATTR_INLINE` | 强制内联 |
| `ATTR_PURE` / `ATTR_CONST` / `ATTR_CONST_INLINE` | 纯函数标注（少用） |
| `LIKELY(x)` / `UNLIKELY(x)` | 分支预测提示 |

```cpp
ATTR_NODISCARD
virtual Value get(std::string_view key);
```

**日志宏：**

```cpp
static Logger* g_logger = GET_LOGGER("system");   // 命名 logger；LOG_ROOT 取根 logger
LOG_INFO(g_logger) << "message";
LOG_FMT_INFO(g_logger, "format %d", value);
```

**断言宏：** `ASSERT(expr, msg)` 运行时断言；`ASSERT_THROW(expr, ex)` 断言失败抛异常；`CHECK_NOTNULL(ptr)` 指针非空检查。

**线程安全注解**（需 `-Wthread-safety` 编译选项）：`GUARDED_BY(x)`、`REQUIRES(...)`，详见 `base/Macro.h`。

## 7. 资源管理（RAII）

- **实体类**（拥有资源 / 唯一身份）继承 `noncopyable`；**值类型**（可拷贝语义，如 `Buffer`、`Timestamp`）继承 `copyable`
- 互斥锁用项目封装：`MutexLock` + `MutexLockGuard`（`thread/Mutex.hpp`），临界区随 `{}` 作用域自动释放
- 所有资源（锁、fd、句柄）用 RAII 对象管理，禁止手动配对 acquire / release

## 8. 完整示例

```cpp
//***********************************************
// name      : Example.h
// brief     : 示例类，演示代码风格规范
// attention : 使用前必须调用 initialize()
//***********************************************
#pragma once
#include <base/noncopyable.hpp>
#include <memory>
#include <string>

namespace bus
{

class Example : noncopyable
{
 public:
    using ptr = std::shared_ptr<Example>;

    /// @brief 初始化对象
    /// @return 成功返回 true，失败返回 false
    /// @attention 必须在其他方法前调用
    ATTR_NODISCARD
    bool initialize() noexcept;

    /// @brief 执行任务（动作方法：PascalCase）
    /// @param value 输入值
    /// @return 处理结果
    ATTR_NODISCARD
    int process(int value) noexcept;

    /// @brief 获取名称（getter：camelCase）
    /// @return 对象名称
    ATTR_PURE_INLINE
    const std::string& getName() const noexcept
    {
        return m_Name;
    }

 private:
    /// @brief 对象名称
    std::string m_Name;
    /// @brief 是否已初始化
    bool m_Initialized;
};

} // namespace bus
```
