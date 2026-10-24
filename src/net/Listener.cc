#include "Listener.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"
#include "log/Logger.h"
#include "Exception.hpp"
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
    if (!m_soket->bind(addr))
    {
        // 构造期失败上抛（Q14/I1：端口占用等不能静默——幽灵启动无 API 可
        // 感知）；已构造成员（Socket/Channel）由异常栈自动析构，fd 不泄漏
        throw Exception::SocketError(("Listener bind failed: " + addr.toString()).c_str());
    }
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
    if (!m_soket->listen())
    {
        // bind 成功后 listen 失败近乎不可能（ENOTSOCK 等）：记错误且不启用
        // 监听事件（避免注册一个永不就绪的 fd）；loop 线程内无法向上抛
        LOG_ERROR(g_log) << "Listener: listen failed, fd=" << m_soket->fd()
                         << ", err=" << strerror(errno);
        return;
    }
    m_channel->EnableReadEvent();
    LOG_INFO(g_log)<<"Listener: fd = [" << m_soket->fd()<<"],listenning";
}

void Listener::Stop(){
    assert(m_loop.Peer().isInLoopThread());
    if (m_stopped)
    {
        return;   // 幂等
    }
    m_stopped = true;
    m_channel->DisableAllEvent();
    // 监听 socket 的 shutdown：内核直接拒绝后续 SYN（不进 accept 队列堆积）；
    // fd 保持有效——销毁仍由 ~Socket 的 RAII 负责（宿主析构时统一回收）
    if (::shutdown(m_soket->fd(), SHUT_RDWR) != 0)
    {
        LOG_ERROR(g_log) << "Listener shutdown error, err=" << strerror(errno);
    }
    LOG_INFO(g_log) << "Listener: fd = [" << m_soket->fd() << "], stopped";
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