#pragma once
#include "noncopyable.hpp"
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

    /// @param loop 所属事件循环（借用指针，装配点保证非空）
    Listener(EventLoop* loop, const Address& addr);

    ~Listener();

    void Listen();

    /// @brief 暂停监听：从 epoll 摘除监听事件并拒绝后续连接（幂等）
    /// @attention 仅摘事件与拒连（shutdown），不销毁对象/fd——对象生命周期归
    ///            宿主（如 TcpServer 析构时统一销毁，避免分派轮内析构
    ///            Channel 导致同轮 pending 事件悬垂，C1）
    void Stop();

    void setNewConnectionCallback(NewConnectionCallback&& cb) {
        m_callback = std::forward<NewConnectionCallback>(cb);
    }

 private:
    void handleRead();

 private:
    /// @brief 所属事件循环（借用，非空契约）
    EventLoop* m_loop;
    std::unique_ptr<Socket> m_soket;
    std::unique_ptr<Channel> m_channel;
    NewConnectionCallback m_callback;
    /// @brief fd 耗尽预留名额（EMFILE 时释放以接受并拒绝一个连接，防 busy loop）
    socket_t m_idle_fd;
    /// @brief 是否已暂停监听（仅 loop 线程）
    bool m_stopped{false};
};

}  // namespace bus