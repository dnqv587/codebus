#pragma once
#include <functional>
#include <string_view>
#include <string>
#include "base/noncopyable.hpp"
#include "net/Address.h"
#include "base/type.h"

namespace bus{

class EventLoop;
class TcpServer:noncopyable
{
public:
    using DoneCallback = std::function<void ()>;

    /// @brief The constructor of a TCPServer
    /// @param loop 
    /// @param addr ip:port
    /// @param thread_num 工作线程数
    TcpServer(EventLoop* loop,std::string_view addr,uint32_t thread_num);

    ~TcpServer();

    void Start();

    void Stop(DoneCallback cb = DoneCallback());

    void setConnectionCallback(const ConnectionCallback& cb) noexcept{
        m_conn_cb = cb;
    }

    void setMessageCallback(const MessageCallback& cb) noexcept{
        m_msg_cb = cb;
    }

private:
    EventLoop* m_loop;
    std::string m_listen_addr;

    ConnectionCallback m_conn_cb;
    MessageCallback m_msg_cb;
    DoneCallback m_done_cb;
};

}