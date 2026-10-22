#include "Listener.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"
#include "log/Logger.h"
#include <assert.h>
#include <error.h>
#include <fcntl.h>
#include <unistd.h>

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
    // 预占一个 fd 名额：EMFILE 时释放它以接受（并立即关闭）一个连接，
    // 使对端得到确定性拒绝（man 2 accept "The special problem of
    // accept()ing when you can't"）
    m_idle_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

Listener::~Listener(){
    // DisableAllEvent 经 UpdateChannel 的 kNone 分支完成 epoll 摘除与 map 移除；
    // 无须再显式 RemoveChannel（那是二次移除，触发 isListeningChannel 断言）
    m_channel->DisableAllEvent();
    if (m_idle_fd >= 0)
    {
        ::close(m_idle_fd);
    }
}

void Listener::Listen(){
    assert(m_loop.Peer().isInLoopThread());
    m_soket->listen();
    m_channel->EnableReadEvent();
    LOG_INFO(g_log)<<"Listener: fd = [" << m_soket->fd()<<"],listenning";
}

void Listener::handleRead(){
    assert(m_loop.Peer().isInLoopThread());

    // 循环 accept 至 EAGAIN：单次读事件排空积压，减少 LT 模式重复唤醒
    while (true)
    {
        auto [sock, addr] = m_soket->accept();
        if(sock > 0){
            if(m_callback){
                std::invoke(m_callback,sock,std::move(addr));
            } else {
                ::close(sock);
                LOG_WARN(g_log) << "Received a new connection request, but it was not processed";
            }
            continue;
        }

        const int err = errno;
        if(err == EAGAIN || err == EWOULDBLOCK){
            break;   // 积压排空：非阻塞 listen fd 的正常返回
        }
        if(err == EINTR){
            continue;   // 防御（Socket::accept 内已重试一层）
        }
        LOG_ERROR(g_log) << "Listener: accept error, err=" << strerror(err);
        if(err == EMFILE){
            // fd 耗尽：释放预留名额接受（并关闭）一个连接——对端得到确定性
            // 拒绝；否则 accept 队列无法腾空，LT 模式将持续唤醒（busy loop）
            ::close(m_idle_fd);
            m_idle_fd = ::accept(m_soket->fd(), nullptr, nullptr);
            ::close(m_idle_fd);
            m_idle_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            LOG_ERROR(g_log) << "Insufficient fd resources, connection rejected";
        }
        break;
    }
}