#pragma once
#include "base/noncopyable.hpp"
#include "base/view.h"
#include "Address.h"
#include "Buffer.hpp"
#include <memory>
#include <string_view>
#include <functional>
#include <atomic>
#include "base/type.h"

namespace bus{

class EventLoop;
class Channel;
class Socket;
class TCPConnection: noncopyable,public std::enable_shared_from_this<TCPConnection>
{
public:
    using ptr = std::shared_ptr<TCPConnection>;
	using CloseCallback = std::function<void(const TCPConnection::ptr)>;
	using WriteCompleteCallback = std::function<void(const TCPConnection::ptr)>;
	using HighWaterMarkCallback = std::function<void(const TCPConnection::ptr, size_t)>;

    enum Status {
        Disconnected  = 0,
        Connecting    = 1,
        Connected     = 2,
        Disconnecting = 3
    };
    
    TCPConnection(View<EventLoop> loop, std::string_view name, socket_t sock, const Address& localAddr, const Address& remoteAddr);

    ~TCPConnection();

    void close();

    /// @brief 优雅半关闭：已排队数据发完后仅关闭写方向（对端收到 EOF）；
    ///        读路径不受影响——对端后续数据仍触发 msg_cb，直至对端关闭或
    ///        调用 close()（I6：旧注释「停止接收新数据」与实现矛盾）
    /// @attention 与 close() 不同，不触发 close_cb；可从任意线程调用
    void shutdown();

    void send(const std::string_view s);
    void send(const void* data,size_t len);
    void send(const Slice& msg);
    void send(Buffer* buf);

    EventLoop* loop() const noexcept{
        return m_loop;
    }

    socket_t fd() const noexcept;

    /// @brief 禁用Nagle算法,避免连续发包出现延迟
    /// @param on 
    void setTcpNoDelay(bool on);

    Status getStatus() const noexcept{
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

    bool isConnected() const noexcept{
        return m_status == Connected;
    }

    void setConnectionCallback(ConnectionCallback cb) noexcept{
        m_conn_cb = cb;
    }

    void setMessageCallback(MessageCallback cb) noexcept{
        m_msg_cb = cb;
    }

    void setCloseCallback(CloseCallback cb) noexcept {
        m_close_cb = cb;
    }

    void setWriteCompleteCallback(WriteCompleteCallback cb) noexcept{
        m_write_cb = cb;
    }

    void setHighWaterMarkCallback(HighWaterMarkCallback cb,size_t mark) noexcept {
        m_hight_water_cb = cb;
        m_hight_water_mark = mark;
    }

    EventLoop* getLoop() const noexcept{
        return m_loop;
    }

    std::string_view name() const noexcept{
        return m_name;
    }

    const Address& localAddr() const noexcept{
        return m_local_addr;
    }

    const Address& remoteAddr() const noexcept{
        return m_remote_addr;
    }

    Status status() const noexcept{
        return m_status;
    }

private:
    void handleRead();
    void handleWrite();
    void handleError();
    void handleClose();
    void sendInLoop(Slice data);
    void shutdownInLoop();

    /// @brief 拷贝数据后排入 loop 线程发送（send 族公共排队路径，D9）
    /// @param data 待发送数据（进入本函数即拷贝入闭包，调用方缓冲区无生命周期约束）
    void queueSendData(std::string_view data);

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
    WriteCompleteCallback m_write_cb;
    HighWaterMarkCallback m_hight_water_cb;
};

}