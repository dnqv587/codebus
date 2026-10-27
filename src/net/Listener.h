#pragma once
#include "noncopyable.hpp"
#include "view.h"
#include "type.h"
#include <functional>
#include <memory>

namespace bus {

class Channel;
class Address;
class EventLoop;
class Socket;
class Listener : noncopyable {
 public:
    using NewConnectionCallback = std::function<void(socket_t, Address)>;

    Listener(View<EventLoop> loop, const Address& addr);

    ~Listener();

    void Listen();

    /// @brief 暂停监听：从 epoll 摘除监听事件并拒绝后续连接（幂等）
    /// @attention 仅摘事件与拒连（shutdown），不销毁对象/fd——对象生命周期归
    ///            宿主（如 TcpServer 析构时统一销毁，避免分派轮内析构
    ///            Channel 导致同轮 pending 事件悬垂，C1）
    void Stop();

    void setNewConnectionCallback(NewConnectionCallback&& cb) { m_callback = std::forward<NewConnectionCallback>(cb); }

 private:
    void handleRead();

 private:
    View<EventLoop> m_loop;
    std::unique_ptr<Socket> m_soket;
    std::unique_ptr<Channel> m_channel;
    NewConnectionCallback m_callback;
    /// @brief fd 耗尽预留名额（EMFILE 时释放以接受并拒绝一个连接，防 busy loop）
    socket_t m_idle_fd;
    /// @brief 是否已暂停监听（仅 loop 线程）
    bool m_stopped{false};
};

}  // namespace bus