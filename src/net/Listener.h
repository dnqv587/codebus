#pragma once
#include "base/noncopyable.hpp"
#include "base/view.h"
#include "base/type.h"
#include <functional>
#include <memory>

namespace bus{

class Channel;
class Address;
class EventLoop;
class Socket;
class Listener: noncopyable
{
public:
    using NewConnectionCallback = std::function<void (socket_t ,Address)>;

    Listener(View<EventLoop> loop,const Address& addr);

    ~Listener();

    void Listen();

    void setNewConnectionCallback(NewConnectionCallback&& cb){
        m_callback = std::forward<NewConnectionCallback>(cb);
    }

private:
    void handleRead();
private:
    View<EventLoop> m_loop;
    std::unique_ptr<Socket> m_soket;
    std::unique_ptr<Channel> m_channel;
    NewConnectionCallback m_callback;
};

}