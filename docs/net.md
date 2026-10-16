# 网络模块（net）

## 功能概述

网络模块是框架的核心，采用 **One Loop Per Thread** 事件驱动模型：每个 IO 线程拥有独立 `EventLoop`，基于 epoll 的 `EventDispatcher` 分发事件，`Channel` 封装文件描述符与事件回调，`Socket` RAII 管理套接字，上层提供 `Listener`（监听）、`TCPConnection`（连接）、`TcpServer`（服务器）三级抽象。

## 设计原理

```
TcpServer ── 创建 ──> Listener（监听 fd + Channel）
                        │ accept
                        ▼
                  TCPConnection（Socket + Channel + 输入/输出 Buffer）
                        │
        ┌───────────────┴────────────────┐
   EventLoop（每个 IO 线程一个）     EventDispatcher（epoll 封装）
        └── Loop(): epoll_wait ──> Channel::HandleEvent() ──> 回调
```

**关键调用链**：

```
EventLoop::Loop()
  -> EventDispatcher::Dispatcher() (epoll_wait)
  -> Channel::HandleEvent() 分发回调
  -> 执行 PendingFunctors (跨线程任务)
```

**线程模型**：

- 每个 `Channel` 只属于一个 `EventLoop`（通过 `View<EventLoop>` 表达借用所有权），**永远不要跨线程共享 Channel**
- 跨线程任务通过 `EventLoop::RunInLoop(callback)` 排队，`Wakeup()` 经 eventfd 唤醒阻塞中的 `epoll_wait`
- `isInLoopThread()` 判断当前是否处于所属 IO 线程

## 使用示例

### Echo 服务器

```cpp
#include "net/EventLoop.h"
#include "net/TcpServer.h"

using namespace bus;

int main() {
    EventLoop loop;

    // 监听 0.0.0.0:8899，4 个工作线程
    TcpServer server(&loop, "0.0.0.0:8899", 4);

    server.setConnectionCallback([](const std::shared_ptr<TCPConnection> conn) {
        std::cout << "new connection: " << conn->name() << std::endl;
    });

    server.setMessageCallback([](const std::shared_ptr<TCPConnection> conn, Buffer* buf) {
        conn->send(buf);   // 原样回显
    });

    server.Start();
    loop.Loop();   // 阻塞运行事件循环
    return 0;
}
```

### 定时执行跨线程任务

```cpp
EventLoop loop;
std::thread producer([&loop] {
    loop.RunInLoop([] {
        // 在 IO 线程中安全执行
    });
});
loop.Loop();
```

## 接口说明

### EventLoop

| 接口 | 说明 |
|---|---|
| `Loop()` | 运行事件循环（阻塞，直至 `Stop()`） |
| `Stop()` | 停止事件循环 |
| `UpdateChannel(Channel*)` / `RemoveChannel(Channel*)` | 注册/移除监听（内部转 Dispatcher） |
| `RunInLoop(cb)` | 将任务排队到 IO 线程执行；当前已在 IO 线程则立即执行 |
| `Wakeup()` | 通过 eventfd 唤醒阻塞中的 epoll_wait |
| `isInLoopThread()` | 当前是否 IO 线程 |

### Channel

| 接口 | 说明 |
|---|---|
| `Channel(View<EventLoop>, fd)` | 构造，绑定所属 Loop 与文件描述符 |
| `HandleEvent()` | 按 `m_OccurEvents` 分派到读/写/错误回调 |
| `setReadCallback()` / `setWriteCallback()` / `setErrorCallback()` | 设置回调 |
| `EnableReadEvent()` / `EnableWriteEvent()` | 注册监听可读/可写 |
| `DisableReadEvent()` / `DisableWriteEvent()` / `DisableAllEvent()` | 取消监听 |
| `ListeningEvent()` / `OccurEvent()` / `setOccurEvent()` | 监听/发生事件集合 |
| `fd()` / `OwnerLoop()` | 文件描述符 / 所属 Loop |

事件常量（`EventType` 命名空间）：`kNone`、`kReadable`、`kWritable`、`kError`。

**注意**：`Channel` 不拥有 fd 的生命周期，fd 的关闭由持有者（如 `Socket`）负责。

### EventDispatcher（epoll 封装）

| 接口 | 说明 |
|---|---|
| `Dispatcher(timeoutMs, activeChannels)` | 等待事件；`-1` 永久等待、`0` 立即返回、`>0` 定时等待；返回发生事件的 Channel 列表 |
| `UpdateChannel(Channel*)` | 更新 Channel 监听（EPOLL_CTL_ADD/MOD） |
| `RemoveChannel(Channel*)` | 移除监听（EPOLL_CTL_DEL） |

抽象基类 `EventDispatcher` + 具体实现 `EpollEventDispatcher`，便于后续扩展其他多路复用后端。

### Address

| 接口 | 说明 |
|---|---|
| `Address(ip, port, ipv6=false)` | 从 IP 字符串 + 端口构造（自动识别 v4/v6） |
| `Address(sockaddr_in / sockaddr_in6)` | 从 POSIX 地址结构构造 |
| `ipString()` / `port()` / `toString()` | 地址信息 |
| `family()` / `isIPV6()` | 协议族 |
| `getSockAddr()` / `getSockAddrInet4()` / `getSockAddrInet6()` | 底层结构访问 |
| `Length()` | 地址结构长度（按协议族） |

内部使用 `std::variant<sockaddr_in, sockaddr_in6>` 统一 v4/v6。另有 `sockaddr_cast<To>(From*)` 做类型安全的地址结构转换。

### Socket

RAII 封装 socket fd（构造即拥有生命周期，析构自动关闭）：

| 接口 | 说明 |
|---|---|
| `Socket(fd)` / `Socket(Family, Type)` | 包装已有 fd / 新建套接字；`Family`：IPV4/IPV6/UNIX；`Type`：TCP/UDP/NIO_TCP/NIO_UDP |
| `bind(addr)` / `listen()` | 绑定 / 监听（backlog = SOMAXCONN） |
| `accept()` | 接受连接，返回 `pair<socket_t, Address>` |
| `connect(addr)` | 发起连接 |
| `close()` | 关闭并释放资源 |
| `read(buf, count)` / `read(Buffer*)` / `readv(iov, count)` | 读 |
| `write(buf, count)` / `writev(iov, count)` | 写 |
| `ShutdownWrite()` | 关闭写通道（半关闭） |
| `setTcpNoDelay(on)` | 禁用 Nagle 算法 |
| `setKeepAlive(on)` | 开启 TCP 保活（不能代替应用层心跳） |
| `setReuseAddr(on)` / `setReuseExclusiveAddr(on)` | 端口复用（后者要求同 ip/port 的 socket 均设置 SO_REUSEPORT） |
| `setNonBlockAndCloseOnExec()` | 非阻塞 + exec 后自动关闭 |
| `getSocketErrno()` | 获取 socket 错误码 |
| `getLocalAddr()` / `getRemoteAddr()` | 本地/远端地址（`std::optional`） |
| `isAvailable()` / `fd()` | 有效性 / 描述符 |

### Listener

| 接口 | 说明 |
|---|---|
| `Listener(View<EventLoop>, addr)` | 构造监听器，绑定所属 Loop |
| `Listen()` | 创建 socket、绑定、监听，注册 Channel |
| `setNewConnectionCallback(cb)` | 新连接回调 `void(socket_t, Address)` |

### TCPConnection

继承 `enable_shared_from_this`，生命周期由 `shared_ptr` 管理：

| 接口 | 说明 |
|---|---|
| `send(string_view)` / `send(data, len)` / `send(Slice)` / `send(Buffer*)` | 发送（写入输出缓冲并注册可写事件，非阻塞） |
| `close()` | 主动关闭 |
| `setTcpNoDelay(on)` | 禁用 Nagle |
| `setConnectionCallback(cb)` / `setMessageCallback(cb)` | 连接建立/消息到达回调 |
| `setCloseCallback(cb)` | 连接关闭回调 |
| `setWriteCompleteCallback(cb)` | 输出缓冲清空回调 |
| `setHighWaterMarkCallback(cb, mark)` | 输出缓冲超过高水位（默认 `128_MB`）回调 |
| `name()` / `fd()` / `localAddr()` / `remoteAddr()` | 连接信息 |
| `status()` / `isConnected()` | 连接状态（`Disconnected/Connecting/Connected/Disconnecting`） |

### TcpServer

| 接口 | 说明 |
|---|---|
| `TcpServer(loop, addr, thread_num)` | 构造；`addr` 为 `"ip:port"`，`thread_num` 为工作线程数 |
| `Start()` | 启动监听与工作线程池 |
| `Stop(cb)` | 停止服务器，`cb` 为完成回调 |
| `setConnectionCallback(cb)` | 新连接回调（`ConnectionCallback`） |
| `setMessageCallback(cb)` | 消息回调（`MessageCallback`） |

### ServerStatus（模板）

`template <bool isAtomic = true> class ServerStatus`：带状态机（`Null → Initializing → Initialized → Running → Stopping → Stopped → Destroy`）的基类，提供 `getStatus()` / `setStatus()` / `StatusToString()`。

## 注意事项

- ⚠️ **重构进行中**：当前工作区 `TcpServer` 实现未完成（构造/启动逻辑半成品），`EventLoopThreadPool` 为空壳（头文件仅有类声明，未进入构建），上述为**目标 API**
- **UDP 未实现**：`Socket::Type` 已预留 `UDP` / `NIO_UDP` 枚举，但协议层尚未支持
- `ConnectionCallback` / `MessageCallback` 定义于 `base/type.h`：`void(const std::shared_ptr<TCPConnection>)` 与 `void(const std::shared_ptr<TCPConnection>, Buffer*)`
- `TcpServer` 停止后销毁前请确保所有连接已释放；`Stop()` 的回调用于确认全部连接关闭后再回收资源
- 所有网络回调（连接/消息/关闭/写完成）均在 IO 线程执行，用户回调内禁止阻塞
