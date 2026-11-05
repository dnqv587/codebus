#pragma once
#include "noncopyable.hpp"
#include "Address.h"
#include "Buffer.hpp"
#include <memory>
#include <vector>
#include <string_view>
#include <functional>
#include <atomic>
#include "type.h"

namespace bus {

class EventLoop;
class Channel;
class Socket;
class TCPConnection : noncopyable, public std::enable_shared_from_this<TCPConnection> {
 public:
    using ptr = std::shared_ptr<TCPConnection>;

    enum Status { Disconnected = 0, Connecting = 1, Connected = 2, Disconnecting = 3 };

    /// @param loop 所属事件循环（借用指针，装配点保证非空）
    TCPConnection(EventLoop* loop, std::string_view name, socket_t sock, const Address& localAddr,
                  const Address& remoteAddr);

    ~TCPConnection();

    void close();

    /// @brief 优雅半关闭：已排队数据发完后仅关闭写方向（对端收到 EOF）；
    ///        读路径不受影响——对端后续数据仍触发 msg_cb，直至对端关闭或
    ///        调用 close()（I6：旧注释「停止接收新数据」与实现矛盾）
    /// @attention 与 close() 不同，不触发 close_cb；可从任意线程调用
    void shutdown();

    void send(const std::string_view s);
    void send(const std::byte* data, size_t len);
    void send(Slice msg);
    void send(Buffer* buf);

    EventLoop* loop() const noexcept {
        return m_loop;
    }

    socket_t fd() const noexcept;

    /// @brief 禁用Nagle算法,避免连续发包出现延迟
    /// @param on
    void setTcpNoDelay(bool on);

    Status getStatus() const noexcept {
        return m_status;
    }

    /// @brief 连接建立：启用读监听并进入 Connected（在创建连接的 loop 线程、
    ///        accept/connect 成功后调用一次）
    /// @attention 成功后触发 ConnectionCallback；非 Disconnected 状态调用将被
    ///            忽略并记警告（TcpServer 装配前的宽容幂等）
    void connectEstablished();

    /// @brief 连接销毁收尾（客户端路径）：连接失败/中断时摘除事件、置终态
    ///        并通知 conn_cb（观察 Disconnected 终态）
    /// @attention 在所属 loop 线程调用；服务端关闭走 handleClose（fd 事件
    ///            触发），客户端失败路径无事件可触发，由 TcpClient 显式调用；
    ///            Disconnected 态调用幂等（早退）
    void connectDestroyed();

    bool isConnected() const noexcept {
        return m_status == Connected;
    }

    void setConnectionCallback(ConnectionCallback cb) noexcept {
        m_conn_cb = cb;
    }

    void setMessageCallback(MessageCallback cb) noexcept {
        m_msg_cb = cb;
    }

    /// @brief 附加用户上下文（生命周期随连接；建议构造/连接期设置、运行期只读）
    void setContext(std::shared_ptr<void> ctx) noexcept { m_context = std::move(ctx); }
    /// @brief 取用户上下文（未设为空）
    ATTR_NODISCARD
    std::shared_ptr<void> getContext() const noexcept {
        return m_context;
    }
    /// @brief 输入缓冲访问（ioLoop 线程内使用——单飞/WS 等上层消费场景）
    ATTR_NODISCARD
    Buffer* inputBuffer() noexcept {
        return m_input_buffer.get();
    }
    /// @brief 排队发送文件段（排输出缓冲之后——响应头先行；handleWrite 排空缓冲后
    ///        sendfile(2) 零拷贝，EAGAIN 保持写事件重驱；完成触发 write_cb）
    /// @param fd 文件描述符（调用方保活——本类不拥）
    /// @attention 跨线程安全同 send（参数拷贝后 RunInLoop）；仅明文通道（TLS H4 走降级链）
    void sendFile(int fd, uint64_t offset, uint64_t count);

    /// @brief 是否有文件段在途（write_cb 内判断流是否完成——释放 fd 前提；loop 线程）
    ATTR_NODISCARD
    bool isFileSending() const noexcept {
        return m_pending_file_fd >= 0;
    }

    /// @brief 连接关闭观察者（多播；主 close_cb 槽位归 TcpServer::removeConnection 不变）
    /// @attention 观察者在 ioLoop 线程触发，不得发送/关闭连接（仅状态清理/注销）
    void addCloseObserver(std::function<void()> ob) {
        m_close_observers.push_back(std::move(ob));
    }
    void setCloseCallback(CloseCallback cb) noexcept {
        m_close_cb = cb;
    }

    void setWriteCompleteCallback(WriteCompleteCallback cb) noexcept {
        m_write_cb = cb;
    }

    void setHighWaterMarkCallback(HighWaterMarkCallback cb, size_t mark) noexcept {
        m_hight_water_cb = cb;
        m_hight_water_mark = mark;
    }

    EventLoop* getLoop() const noexcept {
        return m_loop;
    }

    std::string_view name() const noexcept {
        return m_name;
    }

    const Address& localAddr() const noexcept {
        return m_local_addr;
    }

    const Address& remoteAddr() const noexcept {
        return m_remote_addr;
    }

    Status status() const noexcept {
        return m_status;
    }

 private:
    void handleRead();
    void handleWrite();
    void handleError();
    void handleClose();
    void sendInLoop(const std::byte* data,size_t size);
    void shutdownInLoop();

    /// @brief 拷贝数据后排入 loop 线程发送（send 族公共排队路径，D9）
    /// @param data 待发送数据（进入本函数即拷贝入闭包，调用方缓冲区无生命周期约束）
    void queueSendData(const std::byte* data, size_t size);

    std::string_view StatusEnumToString(TCPConnection::Status status) const noexcept;

 private:
    EventLoop* m_loop;
    std::string m_name;
    const std::unique_ptr<Socket> m_sock;
    const Address m_local_addr;
    const Address m_remote_addr;
    /// @brief 连接状态（跨线程读写：send/shutdown 读 vs handleClose 写，Q30 原子化）
    std::atomic<Status> m_status;
    std::unique_ptr<Channel> m_channel;
    std::unique_ptr<Buffer> m_input_buffer;
    std::unique_ptr<Buffer> m_output_buffer;

    size_t m_hight_water_mark = 128_MB;
    ConnectionCallback m_conn_cb;
    MessageCallback m_msg_cb;
    CloseCallback m_close_cb;
    /// @brief 用户上下文（Q34 清偿；生命周期随连接）
    std::shared_ptr<void> m_context;
    /// @brief 关闭观察者链（多播；主槽位之外）
    std::vector<std::function<void()>> m_close_observers;
    /// @brief sendfile 推进（loop 线程；EAGAIN 保写事件）
    void pumpPendingFile();
    /// @brief 待发文件段（fd<0=无；handleWrite 排空缓冲后推进）
    int m_pending_file_fd = -1;
    /// @brief 待发文件偏移（推进记账）
    uint64_t m_pending_file_off = 0;
    /// @brief 待发剩余字节
    uint64_t m_pending_file_left = 0;
    WriteCompleteCallback m_write_cb;
    HighWaterMarkCallback m_hight_water_cb;
};

}  // namespace bus