# Codebus

一个基于 **C++20** 的 Linux 高性能网络框架库，参考 [muduo](https://github.com/chenshuo/muduo) 与 [sylar](https://github.com/sylar-yin/sylar) 的设计思想，提供事件驱动、多线程、日志、配置、定时器等基础能力，帮助你快速构建高并发的服务器程序。

> ⚠️ **项目状态**：框架处于积极开发中，部分模块仍在完善（UDP、TcpClient、Lua 等）。TCP 服务端栈已完整可用（`TcpServer`/`EventLoopThreadPool`/`EventLoopThread`/`TCPConnection`，支持多 IO 线程与优雅停止），当前工作区可完整编译，核心模块（日志/网络/定时器/配置/缓冲区）测试通过；`Buffer`/`Slice` 已迁移到 `std::span` 只读视图，所有权视图 `View<T>` 已应用于事件模块。详细文档见 [docs/](docs/README.md)。

## 项目简介

Codebus 采用 **One Loop Per Thread** 的经典事件驱动模型：每个 IO 线程拥有独立的 `EventLoop`，基于 `epoll` 分发事件，配合跨线程任务队列实现线程安全的任务调度。框架提供从网络通信到日志落盘的完整基础组件，适合用于：

- 高性能 TCP 服务器 / 网关服务
- 游戏服务器框架底座
- 需要自定义协议的业务服务端
- 学习 muduo/sylar 网络编程思想的参考实现

## 特性

- **事件驱动**：基于 epoll 的 `EventDispatcher`，非阻塞 IO，支持高并发连接
- **One Loop Per Thread**：每个 IO 线程独立 `EventLoop`，无锁化事件处理
- **多线程支持**：线程封装、线程池、互斥锁/读写锁/条件变量/信号量/倒计时门闩等同步原语
- **高效缓冲区**：基于 `std::span<std::byte>` 的 `Slice` + 三区（预留/可读/可写）`Buffer`，支持滑动窗口增长
- **定时器**：基于 `timerfd` 的 `TimerManager`，支持一次性与周期定时任务
- **日志系统**：六级日志、格式化模板、文件滚动、同步/异步落盘、多 Logger 管理
- **配置系统**：基于 tomlc17 的 TOML 配置加载，类型安全取值（`std::optional`），支持点号路径查询
- **现代化 C++20**：`std::span` / `std::variant` / `std::chrono` / 编译期字面量，RAII 资源管理
- **所有权明确**：`View<T>` 视图（借用）/ `std::shared_ptr`（共享）/ `std::unique_ptr`（独占）三种所有权语义

## 模块说明

| 模块 | 目录 | 说明 | 文档 |
|---|---|---|---|
| 基础组件 | [src/base/](src/base/) | 宏、不可拷贝基类、单例、观察者、阻塞队列、所有权视图 | [docs/base.md](docs/base.md) |
| 缓冲区 | [src/Buffer.hpp](src/Buffer.hpp) | Slice 视图 + 三区 Buffer | [docs/buffer.md](docs/buffer.md) |
| 异常 | [src/exception/](src/exception/) | 框架异常类型定义 | [docs/util.md](docs/util.md) |
| 日志 | [src/log/](src/log/) | Logger、格式化器、文件/标准输出 Appender | [docs/log.md](docs/log.md) |
| 网络 | [src/net/](src/net/) | EventLoop、Channel、Socket、TcpServer、TCPConnection | [docs/net.md](docs/net.md) |
| 线程 | [src/thread/](src/thread/) | Thread、ThreadPool、同步原语 | [docs/thread.md](docs/thread.md) |
| 时间 | [src/time/](src/time/) | Timestamp、DateTime（RFC3339） | [docs/time.md](docs/time.md) |
| 定时器 | [src/Timer.h](src/Timer.h) | TimerManager、Timer | [docs/timer.md](docs/timer.md) |
| 配置 | [src/Config.h](src/Config.h) | Config、Value、系统配置 | [docs/config.md](docs/config.md) |
| 工具 | [src/util/](src/util/) | 字符串/时间/系统工具函数 | [docs/util.md](docs/util.md) |

## 环境要求

- **操作系统**：Linux（POSIX）
- **编译器**：GCC 7+ 或 Clang 5+（需 C++20 支持）
- **CMake**：3.10+
- **依赖库**：`pthread`（自动链接）；[tomlc17](https://github.com/cktan/tomlc17)（已内置，位于 `3rd/toml/`）；gtest（已内置，位于 `3rd/gtest/`，仅测试使用）

## 安装说明

### 依赖项

全部第三方依赖已随仓库提供（`3rd/` 目录），无需额外安装。

### 编译步骤

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

- 动态库：`lib/libcodebus.so`
- 静态库：`lib/libtomlc17.a`
- 可执行文件（含测试程序）：`bin/`

### 运行测试

测试基于 gtest 并已接入 CTest，**一单元一测试目标**（目录镜像 src/ 结构，共 36 个：base/exception/log/time/util/thread/net 各模块单元测试 + integration/ 压测），包含网络模块的高并发 Echo、连接风暴、定时器交织与长时浸泡测试：

```bash
cd build
ctest --output-on-failure

# 指定单元
ctest -R BufferTest

# 并发稳定性（重复 10 轮）
ctest -R "NetworkStress|EventLoop|Channel|Listener|Timer" --repeat until-fail:10
```

并发/内存检测可通过 `-DCODEBUS_SANITIZER=thread|address` 使用独立构建目录开启；已知缺陷与疑点登记见 [docs/known-issues.md](docs/known-issues.md)。

## 使用方法

### 基础示例：日志

```cpp
#include "log/Logger.h"

using namespace bus;

// 获取名为 "sys" 的 Logger（配置文件 sys.toml 中定义）
static Logger& g_logger = GET_LOGGER("sys");

int main() {
    // 加载系统配置（自动加载 ./sys.toml，配置 Logger）
    LoggerManager::getInstance()->LoadConfig("sys");

    LOG_INFO(g_logger) << "hello codebus";
    LOG_FMT_WARN(g_logger, "answer = %d", 42);
    return 0;
}
```

### 网络模块：Echo 服务器

```cpp
#include "net/EventLoop.h"
#include "net/TcpServer.h"
#include "log/Logger.h"

using namespace bus;

int main() {
    EventLoop loop;

    // 监听 0.0.0.0:8899，4 个工作线程
    TcpServer server(&loop, "0.0.0.0:8899", 4);

    server.setConnectionCallback([](const std::shared_ptr<TCPConnection> conn) {
        LOG_INFO(GET_LOGGER("sys")) << "new connection: " << conn->name();
    });

    server.setMessageCallback([](const std::shared_ptr<TCPConnection> conn, Buffer* buf) {
        conn->send(buf);   // 原样回显
    });

    server.Start();
    loop.Loop();   // 阻塞运行事件循环
    return 0;
}
```

### 配置模块

```cpp
#include "Config.h"

using namespace bus;

// 系统配置 sys_config 在程序启动时自动加载 ./sys.toml
auto port = SYS_CONF("server.port");
if (auto v = port->asInteger()) {
    std::cout << "server.port = " << *v << std::endl;
}

// 独立的配置文件
Config conf;
conf.LoadFile("conf/app.toml");
std::cout << conf.get("app.name")->asString().value_or("unnamed") << std::endl;
```

### 线程模块

```cpp
#include "thread/Thread.h"
#include "thread/ThreadPool.h"

using namespace bus;

// 单个线程
Thread t([] { /* 任务 */ }, "worker");
t.run();
t.join();

// 线程池
ThreadPool pool("worker-pool");
pool.start(4);               // 4 个线程（默认 CPU 核心数）
pool.run([] { /* 任务 */ });
pool.stop();
```

### 定时器模块

```cpp
#include "net/EventLoop.h"
#include "Timer.h"

using namespace bus;
using namespace std::chrono;

int main() {
    EventLoop loop;
    TimerManager timers(std::ref(loop));

    // 1 秒后触发一次
    timers.addTimer([]{ /* 一次性任务 */ },
                    steady_clock::now() + 1s, milliseconds(0));

    // 每 500ms 周期触发
    auto timer = timers.addTimer([]{ /* 周期任务 */ },
                                 steady_clock::now() + 500ms, milliseconds(500));

    loop.Loop();
    return 0;
}
```

## 目录结构

```
codebus
├── 3rd/          # 第三方依赖（tomlc17、gtest）
├── bin/          # 编译输出：可执行文件与测试程序
├── build/        # CMake 构建目录
├── docs/         # 模块文档（docs/README.md 为索引）
├── lib/          # 编译输出：静态库与动态库
├── src/          # 框架源码
│   ├── base/     # 基础组件（宏、不可拷贝基类、单例、观察者、阻塞队列、View）
│   ├── exception/# 异常类型定义
│   ├── log/      # 日志模块
│   ├── net/      # 网络模块（事件驱动核心）
│   ├── thread/   # 线程模块（线程、线程池、同步原语）
│   ├── time/     # 时间模块（Timestamp、DateTime）
│   ├── util/     # 工具模块
│   ├── Buffer.hpp# 缓冲区（Slice + Buffer）
│   ├── Config.h  # 配置模块（Config + Value）
│   ├── Hook.h    # 系统调用 Hook（规划中，暂未实现）
│   └── Timer.h   # 定时器模块（TimerManager + Timer）
└── test/         # 测试程序（gtest）
```

## 参考资料

- [muduo](https://github.com/chenshuo/muduo) - 高性能 C++ 网络库
- [sylar](https://github.com/sylar-yin/sylar) - 高性能服务器框架
- [tomlc17](https://github.com/cktan/tomlc17) - C 语言 TOML 解析器

## 许可证

项目当前未指定许可证（暂无 LICENSE 文件）。
