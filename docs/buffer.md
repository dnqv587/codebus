# 缓冲区模块（Buffer）

## 功能概述

缓冲区模块提供两个核心类型：

- **`Slice`**：基于 `std::span<std::byte>` 的只读字节视图，零拷贝引用一段内存，不拥有数据
- **`Buffer`**：muduo 风格的"三区"读写缓冲区，用于网络模块收发数据的暂存与粘包处理

## 设计原理

### Slice：span 视图

`Slice` 包装 `std::span<std::byte>`，提供 `data()`、`size()`、`empty()`、`begin()`/`end()`、`operator[]`、`toString()`（转 `std::string_view`）以及三路比较运算符 `<=>`。构造方式：

```cpp
Slice(It data, size_t size);     // 指针/迭代器 + 长度
Slice(It begin, It end);         // 迭代器区间
```

### Buffer：三区结构

```
+-------------------+------------------+------------------+
| prependable bytes |  readable bytes  |  writable bytes  |
|                   |     (CONTENT)    |                  |
+-------------------+------------------+------------------+
0      <=      readerIndex   <=   writerIndex    <=     capacity
```

- **预留区（prependable）**：`CheapPrependSize = 8` 字节，允许在头部廉价插入协议头（如消息长度字段）
- **可读区（readable）**：已写入未消费的数据，`ReadBegin()` 到 `WriteBegin()`
- **可写区（writable）**：可直接写入的空间，`WriteBegin()` 到 `Capacity()`

**增长策略**（`grow()`）：可写空间不足时，若"可写 + 预留"空间够用则滑动窗口把可读数据前移；否则按 `2 * capacity + len` 扩容（`std::vector<std::byte>` 存储）。

## 使用示例

```cpp
#include "Buffer.hpp"
#include <vector>

using namespace bus;

std::vector<std::byte> data{std::byte('h'), std::byte('i'), std::byte('\n')};
Slice slice(data.begin(), 3);

Buffer buf(1024);           // 初始可写 1024 字节
buf.Write(slice);           // 写入 "hi\n"

Slice peek = buf.PeekAll();        // 查看全部可读数据，不消费
Slice line  = buf.Read(3);         // 消费 3 字节
auto  eol   = buf.FindEOL();       // 查找 '\n'（协议分包常用）
```

## 接口说明

### Slice

| 接口 | 说明 |
|---|---|
| `data()` | 返回底层 `std::byte*` |
| `size()` / `empty()` | 字节数 / 是否为空 |
| `peek()` | 返回内部 `std::span<std::byte>` |
| `begin()` / `end()` | 迭代器 |
| `operator[](n)` | 取第 n 字节（assert 越界检查） |
| `toString()` | 转为 `std::string_view` |
| `operator<=>` | 三路比较（比较 data 指针） |
| `operator<<` | 流输出（按字符串输出） |

### Buffer

| 接口 | 说明 |
|---|---|
| `Buffer(capacity)` | 构造，capacity 为初始可写大小 |
| `Write(Slice)` | 追加写入；空间不足自动扩容 |
| `Prepend(Slice)` | 向预留区头部插入（如协议头） |
| `Read(n)` / `ReadAll()` | 消费 n 字节 / 全部可读数据，返回 `Slice` |
| `Peek(n)` / `PeekAll()` | 查看 n 字节 / 全部，不消费 |
| `SkipRead(n)` / `SkipWrite(n)` | 移动读写游标 |
| `Truncate(n)` | 截断：n=0 清空全部，否则保留前 n 字节 |
| `Reset()` | 清空（读写游标复位到 CheapPrependSize） |
| `Reserve(len)` / `EnsureWriteableSize(len)` | 预留 / 保证可写空间 |
| `ReadableSize()` / `WritableSize()` / `PrependableSize()` / `Capacity()` | 各区大小 |
| `FindEOL()` | 在可读区查找首个 `'\n'`，返回迭代器 |
| `begin()` / `end()` / `ReadBegin()` / `WriteBegin()` | 迭代器访问 |

## 注意事项

- `Slice` 是**视图**：它引用的内存必须在 `Slice` 使用期间保持有效；`Buffer` 扩容（`grow`）后旧 `Slice` 可能失效
- 当前重构进行中：`Slice` 暂无法直接从 `const char*` / `const void*` 构造（`std::span<std::byte>` 要求 `std::byte` 类型迭代器），网络模块读写路径需要显式转换，待收尾修复
- `Buffer::WritableSize()` 等接口含 `assert` 检查，仅 Debug 构建生效
- `operator<=>` 比较的是底层 data 指针而非内容，如需内容比较请转 `std::string_view` 后比较
