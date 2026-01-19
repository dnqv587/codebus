# Codebus C++ 代码风格规范

## 基本原则

本项目是一个基于 C++17 的网络框架库。所有生成的代码必须严格遵守以下风格规范，确保代码质量和可维护性。

---

## 1. 文件头注释规范

每个源文件必须包含文件头注释，格式如下：

```cpp
//***********************************************
// name      : <文件名>
// brief     : <文件功能简要描述>
// attention : <注意事项，如果没有则写 None>
//***********************************************
```

**示例：**
```cpp
//***********************************************
// name      : Thread.h
// brief     : 线程封装类，提供线程管理和控制功能
// attention : 线程启动后必须调用 join 或 detach
//***********************************************
#pragma once
```

---

## 2. 命名规范

### 2.1 命名空间
- 使用小写字母，单词间不分割
- 命名空间统一使用 `bus`

```cpp
namespace bus { ... }
```

### 2.2 类名
- 使用 PascalCase（大驼峰）
- 类名应当具有描述性，清晰表达其功能

```cpp
class Thread { ... }
class EventLoop { ... }
class Logger { ... }
```

### 2.3 成员变量
- 使用 m_ 前缀 + PascalCase
- 私有成员变量必须添加注释说明其作用

```cpp
private:
    /// @brief 线程id
    pid_t m_tid;
    /// @brief 线程名
    std::string m_threadName;
    /// @brief 格式化器
    LogFormatter::ptr m_format;
```

### 2.4 函数名
- 使用 PascalCase（大驼峰）
- 函数名应当使用动词或动词短语，清晰表达其行为

```cpp
void run();
void join();
std::string getName() const;
```

### 2.5 常量和宏
- 常量使用 k_ 前缀 + PascalCase 或全大写下划线分隔
- 宏使用全大写下划线分隔

```cpp
#define MAX_SIZE 1024
#define LOG_INFO(logger) ...
```

### 2.6 类型别名
- 使用 using 而非 typedef
- 使用 PascalCase

```cpp
using ptr = std::shared_ptr<Thread>;
using ThreadFunc = std::function<void()>;
using FunctorList = std::vector<Functor>;
```

---

## 3. 函数注释规范

每个公共接口必须包含详细的 Doxygen 风格注释，格式如下：

```cpp
/// @brief <函数功能描述>
/// @param <参数名> <参数说明>
/// @return <返回值说明>
/// @throw <异常类型> <异常说明（如有）>
/// @attention <注意事项（如有）>
```

**完整示例：**
```cpp
/// @brief 启动线程
/// @attention 线程启动后必须调用 join 或 detach，否则会造成资源泄漏
void run();

/// @brief 将线程绑定到CPU固定核心
/// @param cpuid 核心号，必须小于系统核心总数
/// @throw std::runtime_error 绑定失败时抛出异常
void bindCPU(uint32_t cpuid);

/// @brief 获取线程ID
/// @return 线程的 pid_t 类型 ID
ATTR_PURE_INLINE
pid_t getTid() const
{
    return m_tid;
}
```

---

## 4. 代码格式规范

### 4.1 缩进
- 使用 **4 个空格** 缩进，不使用制表符

### 4.2 大括号风格
- K&R 风格：左大括号不换行
- 类定义：左大括号换行

```cpp
// 函数和方法
void run() {
    // code
}

// 类定义
class Thread {
 public:
    void run();
 private:
    int m_id;
};
```

### 4.3 访问修饰符
- 使用 `public:`、`private:`、`protected:`，后加一个空格
- 访问修饰符缩进一级

```cpp
class Thread {
 public:
    void run();

 private:
    int m_id;
};
```

### 4.4 空格使用
- 操作符前后加空格
- 逗号后加空格
- 控制语句关键字后加空格
- 函数名和左括号间不加空格

```cpp
if (condition) {
    result = a + b;
    func(arg1, arg2);
}
```

### 4.5 引用和指针
- 引用和指针符号与类型结合，而非变量名

```cpp
int* ptr;
int& ref;
const std::string& name;
```

### 4.6 include 顺序
1. 对应的头文件
2. 项目头文件（使用相对路径）
3. 标准库头文件
4. 第三方库头文件

```cpp
#include "Thread.h"
#include "util/Util.h"
#include <thread>
#include <memory>
```

---

## 5. C++ 特性使用规范

### 5.1 使用 `#pragma once` 而非 `#ifndef` 保护
```cpp
#pragma once
```

### 5.2 使用 `override` 和 `final` 关键字
```cpp
class Derived : public Base {
public:
    void foo() override;
    void bar() noexcept final;
};
```

### 5.3 使用 `noexcept` 标记不抛异常的函数
```cpp
void safeFunction() noexcept;
```

### 5.4 使用 `std::move` 和 `std::forward` 优化移动语义
```cpp
explicit Thread(ThreadFunc func, std::string threadName)
    : m_func(std::move(func)), m_threadName(std::move(threadName))
{
}
```

### 5.5 使用 `auto` 推导类型
```cpp
auto iter = m_configs.find(configName);
```

### 5.6 使用 `nullptr` 而非 `NULL` 或 `0`
```cpp
if (ptr != nullptr) { ... }
```

### 5.7 使用智能指针
- 使用 `std::shared_ptr` 表示共享所有权
- 使用 `std::unique_ptr` 表示独占所有权
- 使用 `std::weak_ptr` 避免循环引用

```cpp
using ptr = std::shared_ptr<Thread>;
using unique_ptr = std::unique_ptr<Thread>;
using weak_ptr = std::weak_ptr<Thread>;
```

### 5.8 使用 `std::string_view` 代替 `const std::string&`
```cpp
void LoadFile(std::string_view file);
Value get(std::string_view configName);
```

---

## 6. 项目特定的宏和属性

### 6.1 属性宏（定义在 `base/Macro.h`）
必须使用项目定义的属性宏：

| 宏 | 用途 |
|---|---|
| `ATTR_INLINE` | 强制内联 |
| `ATTR_NODISCARD` | 必须检查返回值 |
| `ATTR_PURE` | 纯函数，返回值只依赖参数 |
| `ATTR_CONST` | 纯函数，不依赖全局变量 |
| `ATTR_PURE_INLINE` | 内联纯函数 getter |
| `ATTR_CONST_INLINE` | 内联 const 函数 getter |

**使用示例：**
```cpp
ATTR_PURE_INLINE
std::string getName() const
{
    return m_name;
}

ATTR_NODISCARD
virtual Value get(std::string_view configName);
```

### 6.2 线程安全注解
使用 Clang 线程安全分析注解（需要编译选项 `-Wthread-safety`）：

```cpp
#define REQUIRES(...) \
    THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))

#define GUARDED_BY(x) \
    THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
```

### 6.3 日志宏（定义在 `log/Logger.h`）
使用项目定义的日志宏：

```cpp
#define LOG_INFO(logger)   LOG_LEVEL(logger, LogLevel::INFO)
#define LOG_ERROR(logger)  LOG_LEVEL(logger, LogLevel::ERROR)
#define LOG_FMT_INFO(logger, fmt, ...)  LOG_FMT_LEVEL(logger, LogLevel::INFO, fmt, ##__VA_ARGS__)

#define LOG_ROOT LoggerManager::getInstance()->getRoot()
#define GET_LOGGER(x) LoggerManager::getInstance()->getLogger(x)

// 使用示例
static Logger& g_logger = GET_LOGGER("system");
LOG_INFO(g_logger) << "message";
LOG_FMT_INFO(g_logger, "format %d", value);
```

### 6.4 断言宏
```cpp
#define ASSERT(expr, msg) \
    static_cast<bool>(expr)?void(0):assert_fail(#expr, __FILE__, __LINE__, msg)

#define ASSERT_THROW(expr, ex) \
    (LIKELY(static_cast<bool>(expr))?void(0):throw AssertFail(#expr, StringUtil::BaseFileName(__FILE__), __LINE__, __FUNCTION__, std::forward<decltype(ex)>(ex)))
```

---

## 7. 异常处理规范

### 7.1 异常抛出
- 使用 `throw` 而非抛出异常对象
- 捕获异常后可重新抛出或处理

```cpp
try {
    m_handle = std::make_unique<Handle>(toml::parse_file(file));
} catch (...) {
    throw;
}
```

### 7.2 异常规范
- 使用 `noexcept` 标记不抛异常的函数
- 析构函数应标记为 `noexcept`

```cpp
~Config4Toml() noexcept override = default;
```

---

## 8. RAII 和资源管理

### 8.1 使用 RAII 模式管理资源
```cpp
{
    MutexLockGuard lock(m_mutex);
    // 临界区代码
}  // 自动释放锁
```

### 8.2 线程安全类
使用项目中定义的互斥锁和锁守卫：
```cpp
#include <thread/Mutex.hpp>

class Config4Toml : public ConfigBase {
private:
    MutexLock m_mutex;
    // ...
};

void Config4Toml::LoadFile(std::string_view file) {
    MutexLockGuard lock(m_mutex);
    m_handle = std::make_unique<Handle>(toml::parse_file(file));
}
```

### 8.3 不可复制基类
使用 `noncopyable` 基类禁止复制：

```cpp
class Thread : noncopyable {
    // ...
};
```

---

## 9. 性能优化建议

### 9.1 使用 `std::move` 避免不必要的拷贝
```cpp
explicit ConfigBase(std::string configName)
    : m_Name(std::move(configName))
{
}
```

### 9.2 使用编译器优化提示
```cpp
#define LIKELY(x)      __builtin_expect(!!(x),1)
#define UNLIKELY(x)    __builtin_expect(!!(x),0)

if (UNLIKELY(!m_isLoaded)) {
    throw Exception::ConfigUnLoad();
}
```

### 9.3 按引用传递大对象
```cpp
void addAppender(const LogAppender::ptr& appender);
void addAppender(LogAppender::ptr&& appender);
```

---

## 10. 代码审查检查清单

生成代码时必须确保：
- [ ] 所有文件包含规范的文件头注释
- [ ] 所有公共接口包含完整的 Doxygen 注释
- [ ] 使用正确的命名规范（类、变量、函数）
- [ ] 成员变量使用 m_ 前缀并添加注释
- [ ] 使用 4 空格缩进
- [ ] 正确使用 `override`、`final`、`noexcept`
- [ ] 使用项目定义的属性宏
- [ ] 使用智能指针管理内存
- [ ] 使用 `std::string_view` 代替 `const std::string&`
- [ ] 使用 RAII 管理资源
- [ ] 不可复制类继承 `noncopyable`
- [ ] 符合 C++17 标准

---

## 11. 示例完整代码

```cpp
//***********************************************
// name      : Example.h
// brief     : 示例类，展示代码风格规范
// attention : 必须在使用前初始化
//***********************************************
#pragma once
#include <base/noncopyable.hpp>
#include <string>
#include <memory>
#include <functional>

namespace bus
{

class Example : noncopyable
{
 public:
    using ptr = std::shared_ptr<Example>;
    using Callback = std::function<void(int)>;

    /// @brief 构造函数
    /// @param name 对象名称
    /// @attention 构造后必须调用 initialize()
    explicit Example(std::string name);

    /// @brief 析构函数
    ~Example() noexcept override = default;

    /// @brief 初始化对象
    /// @return 成功返回 true，失败返回 false
    bool initialize() noexcept;

    /// @brief 执行任务
    /// @param value 输入值
    /// @return 处理后的结果
    /// @attention 必须在 initialize() 后调用
    ATTR_NODISCARD
    int process(int value) noexcept;

    /// @brief 获取名称
    /// @return 对象名称
    ATTR_PURE_INLINE
    const std::string& getName() const noexcept
    {
        return m_name;
    }

    /// @brief 设置回调函数
    /// @param cb 回调函数对象
    void setCallback(Callback cb);

 private:
    /// @brief 内部处理函数
    /// @param value 输入值
    /// @return 处理结果
    int doProcess(int value) noexcept;

    /// @brief 对象名称
    std::string m_name;
    /// @brief 是否已初始化
    bool m_initialized;
    /// @brief 回调函数
    Callback m_callback;
};

} // namespace bus
```

---

## 12. 文档规范

### 12.1 API 文档
每个类和公共接口必须在 AI 目录下生成对应的文档，包括：
- 功能概述
- 设计原理
- 使用示例
- 接口说明（参数、返回值、异常）
- 注意事项

### 12.2 GitHub 风格文档
- 使用 Markdown 格式
- 代码块使用语法高亮
- 包含完整的示例代码
- 提供清晰的目录结构
- 使用规范的 Markdown 标题层级
