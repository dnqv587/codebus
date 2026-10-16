# 基础组件模块（base）

## 功能概述

base 模块提供框架最底层的通用组件：编译器属性宏、分支预测、断言、不可拷贝/可拷贝基类、单例模板、所有权视图 `View<T>`、阻塞队列、观察者模式以及基础类型定义。所有其他模块均构建于该模块之上。

## 组件说明

### 1. 宏与属性（`Macro.h`）

| 宏 | 说明 |
|---|---|
| `LIKELY(x)` / `UNLIKELY(x)` | 分支预测提示（`__builtin_expect`） |
| `ASSERT(expr, msg)` | 运行时断言，失败打印文件/行号/消息并 `abort()` |
| `CONSTEVAL(x)` / `GET_CONSTEVAL(x)` | 常量表达式惰性求值 |
| `ATTR_INLINE` | 强制内联 |
| `ATTR_NODISCARD` | 返回值必须使用（`[[nodiscard]]`） |
| `ATTR_PURE` / `ATTR_CONST` | 纯函数声明（仅 Release 生效） |
| `ATTR_PURE_INLINE` / `ATTR_CONST_INLINE` | getter 组合属性：内联 + nodiscard + 纯函数 |
| `ARRT_FORMAT(type, fmt, arg)` | printf 风格格式化检查 |
| `CASE_STRING_BEGIN/CASE_STRING/CASE_STRING_END` | 枚举转字符串 switch 简写 |

线程安全注解（Clang 专用，需 `-Wthread-safety`，GCC 下为空操作）：`CAPABILITY`、`GUARDED_BY`、`PT_GUARDED_BY`、`REQUIRES`、`REQUIRES_SHARED`、`ACQUIRE`、`RELEASE`、`EXCLUDES`、`ASSERT_CAPABILITY`、`NO_THREAD_SAFETY_ANALYSIS` 等，用法见 [Clang ThreadSafetyAnalysis](https://clang.llvm.org/docs/ThreadSafetyAnalysis.html)。

### 2. 基类

| 类 | 说明 |
|---|---|
| `noncopyable` | 不可拷贝基类，拷贝构造/赋值已 `delete` |
| `copyable` | 显式可拷贝基类（默认拷贝语义） |
| `less_than_comparable<T>` | 派生类实现 `operator<` 后自动获得 `>`、`>=`、`<=` |

### 3. `Singleton<T>`

线程安全的懒加载单例（Meyers Singleton），通过 `Singleton<T>::getInstance()` 获取 `T*`。`LoggerManager` 即基于它实现。

### 4. `View<T>` 所有权视图（`view.h`）

参考 sylar 的所有权语义设计，表示"借用但不拥有"的引用关系，内部包装 `std::reference_wrapper<T>`：

```cpp
EventLoop loop;
View<EventLoop> view(std::ref(loop));   // 借用，不拥有生命周期
view.Peer();          // 获取 T&
view.NativeHandle();  // 获取 T*
```

**约定**：`Channel`、`Listener` 等组件使用 `View<EventLoop>` 持有所属 EventLoop，明确表达"不拥有所有权"这一语义。使用 `View<T>` 时须确保被引用对象的生命周期长于视图本身。

### 5. 阻塞队列（`BlockingQueue.hpp`）

| 类 | 说明 |
|---|---|
| `BlockingQueue<T>` | 无界阻塞队列。`put()` 入队、`take()` 出队（空则阻塞）、`drain()` 清空并取出全部元素、`over()` 解除所有阻塞 |
| `BoundBlockingQueue<T>` | 有界阻塞队列。额外提供 `isFull()`；队列满时 `put()` 阻塞，空时 `take()` 阻塞 |

两者内部均使用 `MutexLock` + `Condition` 实现，线程安全。

### 6. 观察者模式（`Observer.hpp`）

`Observable`（被观察者）/ `Observer`（观察者）实现经典观察者模式：`Observer::Observe()` 注册到 `Observable`，`Observable::Notify()` 遍历通知所有观察者并自动清理已失效的 `weak_ptr`。

> ⚠️ **注意**：该文件位于 `namespace uf`（而非 `bus`），是已知的命名空间不一致问题。

### 7. 基础类型（`type.h`）

| 类型/函数 | 说明 |
|---|---|
| `socket_t` | socket 描述符类型（`int`） |
| `event_t` | 事件集合类型（`uint32_t`） |
| `implicit_cast<To>(from)` | 显式调用隐式类型转换 |
| `is_one_of<T, U...>` | 编译期判断 T 是否为 U... 之一 |
| `ConnectionCallback` | `std::function<void(const std::shared_ptr<TCPConnection>)>` |
| `MessageCallback` | `std::function<void(const std::shared_ptr<TCPConnection>, Buffer*)>` |
| `_KB` / `_MB` / `_GB` / `_TB` 字面量 | 文件大小字面量：`4_MB`、`128_MB` |

### 8. 回调别名（`callback.h`）

提供 `EventCallback`、`ReadEventCallback` 等通用回调别名。

> ⚠️ **注意**：该文件同样位于 `namespace uf`，与 `Observer.hpp` 存在相同的命名空间不一致问题。

### 9. 反射宏（`MacroReflaction.hpp`）

配合 `LogFormatter` 使用的代码生成宏，见 [log.md](log.md)。

## 使用示例

```cpp
#include "base/BlockingQueue.hpp"
#include "base/Singleton.hpp"
#include "base/view.h"

using namespace bus;

// 阻塞队列
BlockingQueue<int> queue;
queue.put(42);
int value = queue.take();   // 42

// 单例
class MyManager {
 public:
    void hello() {}
};
MyManager* mgr = Singleton<MyManager>::getInstance();

// 所有权视图
EventLoop loop;
View<EventLoop> view(std::ref(loop));
```

## 注意事项

- `BoundBlockingQueue` 构造函数中的边界检查条件疑似写反（`m_maxSize > 0` 时抛异常），当前实现无法用正数容量构造，待修复
- `Observer.hpp` 与 `callback.h` 的命名空间（`uf`）与项目规范（`bus`）不一致
- 使用 `View<T>` 需自行保证被引用对象生命周期；它不会延长对象生命周期
- `ATTR_PURE` / `ATTR_CONST` 仅在 Release（`NDEBUG`）构建生效
