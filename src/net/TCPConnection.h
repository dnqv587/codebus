#pragma once
#include "base/noncopyable.hpp"
#include "base/view.h"
#include "Address.h"
#include "Buffer.hpp"
#include <memory>
#include <string_view>
#include <functional>
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

    std::string_view StatusEnumToString(TCPConnection::Status status) const noexcept;

private:
    EventLoop* m_loop;
    std::string m_name;
    const std::unique_ptr<Socket> m_sock;
    const Address m_local_addr;
    const Address m_remote_addr;
    Status m_status;
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