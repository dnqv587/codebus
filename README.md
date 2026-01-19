# Codebus

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.0+-blue.svg)](https://cmake.org/)

## 项目简介

Codebus 是一个基于 C++17 的高性能网络框架库，参考了著名的 muduo 和 sylar 框架设计。它提供了一套完整的网络编程解决方案，包括事件驱动、线程管理、日志系统、配置管理等核心功能，旨在帮助开发者快速构建稳定、高效的网络应用。

### 核心特性

- **事件驱动架构**：基于 epoll 实现高效的事件循环
- **线程安全**：完善的线程同步机制和线程池支持
- **高性能日志**：支持同步/异步日志，多级别日志控制
- **灵活配置**：基于 TOML 的配置文件解析系统
- **现代 C++**：充分利用 C++17 特性，代码简洁高效
- **跨平台**：支持 Linux 平台，可扩展至其他 POSIX 系统

## 模块说明

### 网络模块 (`net/`)
实现网络通信的核心功能：
- **EventLoop**：事件循环，管理 I/O 事件
- **Channel**：文件描述符事件通道
- **EventDispatcher**：事件分发器（基于 epoll）
- **TcpServer**：TCP 服务器封装
- **Socket**：套接字封装
- **Address**：网络地址管理

### 事件模块
基于 epoll 的事件驱动机制，实现高效的事件处理和分发。

### 线程模块 (`thread/`)
封装线程相关的功能：
- **Thread**：线程封装，支持 CPU 核心绑定
- **ThreadPool**：线程池，支持任务队列
- **Mutex**：互斥锁（支持读写锁）
- **Condition**：条件变量
- **Semaphore**：信号量
- **CurrentThread**：当前线程信息
- **ProcessInfo**：进程信息

### 定时器模块
实现定时器功能，用于定时任务调度。

### 日志模块 (`log/`)
实现日志功能，记录系统运行时的信息：
- **Logger**：日志记录器
- **LogAppender**：日志输出器（控制台/文件）
- **LogFormatter**：日志格式化器
- **Logging**：日志流式接口

### 配置模块 (`config/`)
实现配置文件的读取和解析功能：
- **Config4Toml**：基于 TOML 格式的配置解析
- **Value**：配置值抽象
- 支持 TOML 格式的配置文件

### 基础模块 (`base/`)
基础组件和工具：
- **noncopyable**：禁止复制的基类
- **copyable**：可复制基类
- **Singleton**：单例模式
- **callback**：回调函数类型定义
- **Macro**：常用宏定义

### 工具模块 (`util/`)
实用工具类：
- **Util**：字符串处理等工具函数

### 时间模块 (`time/`)
时间相关功能：
- **Timestamp**：时间戳
- **DateTime**：日期时间
- **Time**：时间

## 环境要求

- **编译器**：支持 C++17 的编译器（GCC 7+, Clang 5+）
- **CMake**：3.0 或更高版本
- **操作系统**：Linux（推荐 CentOS 7+, Ubuntu 18.04+）
- **依赖库**：
  - libuv（可选，用于跨平台支持）
  - toml++（TOML 配置解析）

## 安装说明

### 依赖项

在编译前，请确保已安装以下依赖：

```bash
# Ubuntu/Debian
sudo apt-get install build-essential cmake

# CentOS/RHEL
sudo yum install gcc-c++ make cmake
```

### 获取源码

```bash
git clone https://github.com/yourusername/codebus.git
cd codebus
```

### 编译步骤

```bash
# 创建构建目录
mkdir build && cd build

# 配置编译选项（Debug 模式）
cmake ..

# 或指定为 Release 模式
cmake -DCMAKE_BUILD_TYPE=Release ..

# 编译
make -j$(nproc)
```

编译完成后：
- 静态库和动态库将输出到 `../lib/` 目录
- 可执行文件（测试程序）将输出到 `../bin/` 目录

### 可选编译选项

在 CMake 配置时可以添加以下选项：

```bash
# 开启详细编译信息
cmake -DCMAKE_VERBOSE_MAKEFILE=ON ..

# 添加自定义编译选项
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON ..
```

## 使用方法

### 基础示例

#### 日志模块使用

```cpp
#include <log/Logger.h>
#include <config/config.h>

using namespace bus;

int main() {
    // 加载配置文件
    ConfigManager::getInstance()->Load("root", "config.toml");
    LoggerManager::getInstance()->LoadConfig("root");

    // 获取日志记录器
    Logger& logger = GET_LOGGER("app");

    // 记录不同级别的日志
    LOG_TRACE(logger) << "这是一条 TRACE 日志";
    LOG_DEBUG(logger) << "这是一条 DEBUG 日志";
    LOG_INFO(logger) << "这是一条 INFO 日志";
    LOG_WARN(logger) << "这是一条 WARN 日志";
    LOG_ERROR(logger) << "这是一条 ERROR 日志";
    LOG_FATAL(logger) << "这是一条 FATAL 日志";

    // 使用格式化日志
    LOG_FMT_INFO(logger, "用户 %s 登录成功，ID: %d", "张三", 12345);

    return 0;
}
```

#### 线程模块使用

```cpp
#include <thread/Thread.h>
#include <thread/ThreadPool.h>
#include <log/Logger.h>

using namespace bus;

void workerTask(int id) {
    Logger& logger = GET_LOGGER("worker");
    LOG_INFO(logger) << "工作线程 " << id << " 正在执行任务";
    // 执行任务逻辑...
    LOG_INFO(logger) << "工作线程 " << id << " 任务完成";
}

int main() {
    LoggerManager::getInstance()->LoadConfig("root");

    // 使用 Thread 类
    Thread thread([](){
        Logger& logger = GET_LOGGER("main");
        LOG_INFO(logger) << "线程正在运行";
    }, "MyThread");

    thread.run();
    thread.join();

    // 使用 ThreadPool 类
    ThreadPool pool("MyThreadPool");
    pool.start(4);  // 启动 4 个工作线程

    // 提交任务到线程池
    for (int i = 0; i < 10; ++i) {
        pool.run([i]() {
            workerTask(i);
        });
    }

    pool.stop();
    return 0;
}
```

#### 网络模块使用

```cpp
#include <net/EventLoop.h>
#include <net/Channel.h>
#include <log/Logger.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace bus;

int main() {
    LoggerManager::getInstance()->LoadConfig("root");

    // 创建监听 socket
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(8888);
    serv.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd, (struct sockaddr*)&serv, sizeof(serv));
    listen(fd, 128);

    // 创建事件循环
    EventLoop loop;
    Channel channel(std::ref(loop), fd);

    // 设置读回调
    channel.setReadCallback([fd, &loop]() {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int cfd = accept(fd, (struct sockaddr*)&client, &len);

        // 为新连接创建 Channel
        auto* chan = new Channel(std::ref(loop), cfd);
        chan->setReadCallback([cfd]() {
            char buf[1024];
            int n = read(cfd, buf, sizeof(buf));
            if (n > 0) {
                // 处理数据
            }
        });
        chan->EnableReadEvent();
        loop.UpdateChannel(chan);
    });

    channel.EnableReadEvent();
    loop.UpdateChannel(&channel);

    // 启动事件循环
    loop.Loop();

    return 0;
}
```

#### 配置模块使用

```cpp
#include <config/config.h>
#include <log/Logger.h>

using namespace bus;

int main() {
    // 加载配置文件
    ConfigManager::getInstance()->Load("app", "config.toml");

    // 获取配置项
    auto value = GET_ROOT_CONFIG("server.port");
    if (value) {
        auto port = value.asInteger();
        if (port) {
            std::cout << "服务器端口: " << *port << std::endl;
        }
    }

    // 获取字符串配置
    auto dbConfig = GET_ROOT_CONFIG("database.host");
    if (dbConfig && dbConfig->isString()) {
        auto host = dbConfig->asString();
        if (host) {
            std::cout << "数据库主机: " << *host << std::endl;
        }
    }

    return 0;
}
```

示例配置文件 `config.toml`：

```toml
[server]
port = 8888
host = "0.0.0.0"

[database]
host = "localhost"
port = 3306
username = "root"
password = "password"
```

## 目录结构

```
codebus/
├── src/              # 源代码目录
│   ├── base/         # 基础组件
│   ├── config/       # 配置模块
│   ├── log/          # 日志模块
│   ├── net/          # 网络模块
│   ├── thread/       # 线程模块
│   ├── time/         # 时间模块
│   └── util/         # 工具模块
├── test/             # 测试代码
├── 3rd/              # 第三方库
│   ├── toml/         # TOML 解析库
│   └── libuv/        # libuv 库
├── docs/             # 文档目录
│   └── AI/           # AI 生成的文档
├── build/            # 编译输出目录
├── bin/              # 可执行文件
├── lib/              # 库文件
├── CMakeLists.txt    # CMake 配置文件
└── README.md         # 项目说明
```

## 参考资料

本项目的设计参考了以下优秀的开源项目：

- [muduo](https://github.com/chenshuo/muduo) - 高性能 C++ 网络库
- [sylar](https://github.com/sylar-yin/sylar) - 高性能服务器框架

## 许可证

本项目采用 MIT 许可证，详见 [LICENSE](LICENSE) 文件。

## 贡献

欢迎提交 Issue 和 Pull Request！

## 联系方式

如有问题或建议，欢迎通过 Issue 联系。
