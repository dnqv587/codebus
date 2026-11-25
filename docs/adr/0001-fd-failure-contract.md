# fd 资源失败契约

Socket(Family,Type) 创建失败立即抛 SocketError（fail-fast，替代 fd=-1 照常构造、延迟死于 assert/误导 bind 错因）；析构容忍 INVALID_SOCKET（跳过 close）、close() 幂等返 true——根治「兜底抛异常后栈展开析构触发 assert」的派生死点（Q32）。借用 fd 构造 Socket(socket_t) 保持宽松（TCPConnection 包装已接受 fd）。EpollEventDispatcher 构造失败走 LOG_FATAL+Abort 而非抛异常：兑现 EventLoopThread 既有声明、与 wakeup fd 先例同款。fd 0 边缘（CreateSocket/isAvailable/操作 assert 均 >0 语义）2026-11-24 裁决维持现状，登记为已知局限。

## Considered Options

- epoll 失败抛异常：否决——EventLoopThread::threadFunc 内 EventLoop 构造无 catch，会走线程 terminate 契约（见 ADR-0002）且无 FATAL 日志证据。
- 操作内 assert 统一为 `!= INVALID_SOCKET`：用户裁决否决，保持 `>0` 与 fd 0 边缘现状一致。