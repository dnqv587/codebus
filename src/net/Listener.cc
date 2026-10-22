#include "Listener.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"
#include "log/Logger.h"
#include <assert.h>
#include <error.h>

using namespace bus;

static Logger& g_log = GET_LOGGER("sys");

Listener::Listener(View<EventLoop> loop,const Address& addr)
:m_loop(loop),
m_soket(std::make_unique<Socket>(addr.isIPV6()?Socket::IPV6:Socket::IPV4,Socket::NIO_TCP)),
m_channel(std::make_unique<Channel>(loop,m_soket->fd()))
{
    m_soket->setReuseAddr(true);
    m_soket->bind(addr);
    m_channel->setReadCallback([this](){
        handleRead();
    });
}

Listener::~Listener(){
    // DisableAllEvent 经 UpdateChannel 的 kNone 分支完成 epoll 摘除与 map 移除；
    // 无须再显式 RemoveChannel（那是二次移除，触发 isListeningChannel 断言）
    m_channel->DisableAllEvent();
}

void Listener::Listen(){
    assert(m_loop.Peer().isInLoopThread());
    m_soket->listen();
    m_channel->EnableReadEvent();
    LOG_INFO(g_log)<<"Listener: fd = [" << m_soket->fd()<<"],listenning";
}

void Listener::handleRead(){
    assert(m_loop.Peer().isInLoopThread());
    
    auto [sock, addr] = m_soket->accept();
    if(sock > 0){
        if(m_callback){
            std::invoke(m_callback,sock,std::move(addr));
            // m_callback(sock,addr);
            LOG_WARN(g_log) << "Connection OK, IP:"<< addr.ipString() << ":"<<addr.port();
        } else {
            close(sock);
            LOG_WARN(g_log) << "Received a new connection request, but it was not processed, IP:"<< addr.ipString() << ":"<<addr.port();
        }
    } else {
        LOG_ERROR(g_log)<< "Listener: handleRead error";
        // TODO: 预留存资源
        // 文件描述符资源不足
        if(errno == EMFILE){
            LOG_ERROR(g_log)<< "Insufficient fd resources";
        }
    }

}