#include "TCPConnection.h"
#include "Channel.h"
#include "log/Logger.h"
#include "Macro.h"
#include "Socket.h"
#include "net/EventLoop.h"

using namespace bus;

Logger* g_log = GET_LOGGER("sys");

TCPConnection::TCPConnection(View<EventLoop> loop, std::string_view name, socket_t sock, const Address& localAddr,
                             const Address& remoteAddr)
    : m_loop(loop.NativeHandle()),
      m_name(name),
      m_sock(std::make_unique<Socket>(sock)),
      m_local_addr(localAddr),
      m_remote_addr(remoteAddr),
      m_status(TCPConnection::Connecting),  // M1：初始态 Connecting，
                                            // Disconnected 成为严格终态
      m_channel(std::make_unique<Channel>(loop, sock)),
      m_input_buffer(std::make_unique<Buffer>(1024)),
      m_output_buffer(std::make_unique<Buffer>(1024)) {
    m_channel->setReadCallback([this]() { handleRead(); });
    m_channel->setWriteCallback([this]() { handleWrite(); });
    m_channel->setErrorCallback([this]() { handleError(); });

    m_sock->setKeepAlive(true);
    LOG_INFO(g_log) << "TCPConnection [" << m_name << "] " << " fd=" << m_sock->fd()
                    << ", local addr=" << m_local_addr.toString() << ", peer Addr" << m_remote_addr.toString()
                    << " status:" << StatusEnumToString(m_status);
}

TCPConnection::~TCPConnection() {
    LOG_INFO(g_log) << "~TCPConnection [" << m_name << "] at" << this << " fd=" << m_sock->fd() << " status"
                    << StatusEnumToString(m_status);
    // M1：Disconnected=已正常关闭；Connecting=从未建立（无事件注册），两者
    // 析构均安全——仍拒绝 Connected/Disconnecting（带事件存活的析构）
    assert(m_status == TCPConnection::Disconnected || m_status == TCPConnection::Connecting);
}

std::string_view TCPConnection::StatusEnumToString(TCPConnection::Status status) const noexcept {
    CASE_STRING_BEGIN(status);
    CASE_STRING(Disconnected);
    CASE_STRING(Connecting);
    CASE_STRING(Connected);
    CASE_STRING(Disconnecting);
    CASE_STRING_END();
}

void TCPConnection::handleRead() {
    assert(m_loop->isInLoopThread());
    ssize_t n = m_sock->read(m_input_buffer.get());
    if (n > 0) {
        std::invoke(m_msg_cb, shared_from_this(), m_input_buffer.get());
    } else if (n == 0) {
        handleClose();
    } else {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
            // 非阻塞读的正常节奏（数据读尽/被信号打断），不按错误记录
            LOG_TRACE(g_log) << "this=" << this << " TCPConnection::handleRead errno=" << err << " " << strerror(err);
        } else {
            handleError();
        }
    }
}

void TCPConnection::handleWrite() {
    assert(m_loop->isInLoopThread());
    Slice buf = m_output_buffer->PeekAll();
    if (buf.size() == 0) {
        // M2：无可写数据（排水完成与写事件就绪的竞态/伪唤醒）——直接收尾，
        // 避免 write(fd,ptr,0)==0 返回 0 落入错误分支读残留 errno 误关连接
        m_channel->DisableWriteEvent();
        return;
    }
    ssize_t n = m_sock->write(buf.data(), buf.size());
    if (n > 0) {
        m_output_buffer->SkipRead(n);
        if (m_output_buffer->ReadableSize() == 0) {
            m_channel->DisableWriteEvent();
            // 半关闭挂起：shutdown 置 Disconnecting 后数据才排空，此时关写方向
            if (m_status == Disconnecting) {
                m_sock->ShutdownWrite();
            }
            if (m_write_cb) {
                m_loop->RunInLoop([conn = shared_from_this()]() { std::invoke(conn->m_write_cb, conn); });
            }
        }
    } else {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
            // 非阻塞写的正常节奏（发送缓冲满），不按错误记录
            LOG_TRACE(g_log) << "this=" << this << " TCPConnection::handleWrite errno=" << err << " " << strerror(err);
        } else {
            handleError();
        }
    }
}

void TCPConnection::handleError() {
    assert(m_loop->isInLoopThread());
    int err = m_sock->getSocketErrno();
    LOG_ERROR(g_log) << "handleError [" << m_name << "] errno=" << err << " " << strerror(err);
    m_status = Disconnecting;
    handleClose();
}

void TCPConnection::handleClose() {
    assert(m_loop->isInLoopThread());
    LOG_INFO(g_log) << "handle close [" << m_name << "] " << " fd=" << m_sock->fd()
                    << ", local addr=" << m_local_addr.toString() << ", peer Addr" << m_remote_addr.toString()
                    << " status:" << StatusEnumToString(m_status);
    if (m_status == Disconnected) {
        return;
    }
    // 被动关闭：Connected 经 handleRead 读 0 直入；主动关闭：close() 先置 Disconnecting
    assert(m_status == Disconnecting || m_status == Connected);
    m_channel->DisableAllEvent();
    // 终态须在回调之前写入（D38）：conn_cb 观察断开，close_cb 可能销毁本对象
    m_status = Disconnected;
    if (m_conn_cb) {
        std::invoke(m_conn_cb, shared_from_this());  // Disconnected 通知（muduo 对齐）：
    }  // 客户端与服务端用户均可观察连接关闭
    if (m_close_cb) {
        std::invoke(m_close_cb, shared_from_this());  // 最后一步（D38：其后零成员访问）
    }
}

void TCPConnection::connectEstablished() {
    assert(m_loop->isInLoopThread());
    if (m_status != Connecting) {
        LOG_WARN(g_log) << "connectEstablished [" << m_name << "] ignored: status=" << StatusEnumToString(m_status)
                        << " (expect Connecting)";
        return;
    }
    m_status = Connected;
    // D27：分派期间连接存活（HandleEvent 守卫持 shared_from_this）
    m_channel->tie(shared_from_this());
    m_channel->EnableReadEvent();
    if (m_conn_cb) {
        std::invoke(m_conn_cb, shared_from_this());
    }
}

void TCPConnection::connectDestroyed() {
    assert(m_loop->isInLoopThread());
    if (m_status == Disconnected) {
        return;  // 幂等：已收尾
    }
    assert(m_status == Connecting || m_status == Connected || m_status == Disconnecting);
    m_channel->DisableAllEvent();  // kNone 快速路径：从未启用的 Channel 亦安全
    // 终态前置（D38 纪律）：conn_cb 观察终态，回调后零成员访问
    m_status = Disconnected;
    if (m_conn_cb) {
        std::invoke(m_conn_cb, shared_from_this());
    }
}

void TCPConnection::close() {
    if (m_status == Disconnected)  // M1：终态不再写回——保持 handleClose 的
    {                              // Disconnected 早退有效（Q15 二次回调源）
        return;
    }
    LOG_INFO(g_log) << "close [" << m_name << "] " << " fd=" << m_sock->fd()
                    << ", local addr=" << m_local_addr.toString() << ", peer Addr" << m_remote_addr.toString()
                    << " status:" << StatusEnumToString(m_status);
    m_status = Disconnecting;
    m_loop->RunInLoop([conn = shared_from_this()]() {
        assert(conn->m_loop->isInLoopThread());
        conn->handleClose();
    });
}

void TCPConnection::shutdown() {
    if (m_status == Disconnected)  // M1：已全关无须半关（终态不可逆）
    {
        return;
    }
    // 优雅半关闭：仅关写方向；待发数据在 loop 线程排空后再关
    // （Q30：m_status 原子化，跨线程置 Disconnecting 安全）
    m_status = Disconnecting;
    m_loop->RunInLoop([conn = shared_from_this()]() {
        assert(conn->m_loop->isInLoopThread());
        conn->shutdownInLoop();
    });
}

void TCPConnection::shutdownInLoop() {
    assert(m_loop->isInLoopThread());
    if (m_status == Disconnected) {
        return;  // 已全关，无须半关
    }
    m_status = Disconnecting;
    if (!m_channel->isListeningWrite() && m_output_buffer->ReadableSize() == 0) {
        m_sock->ShutdownWrite();  // 无待发数据：立即关写方向
    }
    // 否则：handleWrite 排空输出缓冲后补关（见 handleWrite 排空分支）
}

void TCPConnection::send(const std::string_view s) {
    queueSendData(s);
}

void TCPConnection::send(const void* data, size_t len) {
    queueSendData(std::string_view(static_cast<const char*>(data), len));
}

void TCPConnection::send(const Slice& msg) {
    queueSendData(msg.toString());
}

void TCPConnection::send(Buffer* buf) {
    if (m_status != Connected || buf == nullptr) {
        return;
    }
    if (m_loop->isInLoopThread()) {
        // loop 线程：同步消费可读区（无生命周期窗口）
        sendInLoop(buf->PeekAll());
        buf->SkipRead(buf->ReadableSize());
    } else {
        // 非 loop 线程：立即拷贝数据入队并消费——调用方的 Buffer 在 send
        // 返回后即可安全复用/销毁（数据所有权转移语义）
        queueSendData(buf->PeekAll().toString());
        buf->SkipRead(buf->ReadableSize());
    }
}

void TCPConnection::queueSendData(std::string_view data) {
    if (m_status != Connected) {
        return;
    }
    // 拷贝数据后排入 loop（D9）：调用方缓冲区在 send 返回后无生命周期约束
    m_loop->RunInLoop([conn = shared_from_this(), data = std::string(data)]() { conn->sendInLoop(Slice(data)); });
}

void TCPConnection::sendInLoop(Slice data) {
    assert(m_loop->isInLoopThread());
    if (m_status == Disconnected) {
        LOG_WARN(g_log) << "disconnected, give up writing";
        return;
    }
    ssize_t written = 0;
    size_t remain = data.size();
    // channel未监听写事件，且output buffer无数据。直接写
    if (!m_channel->isListeningWrite() && m_output_buffer->ReadableSize() == 0) {
        written = m_sock->write(data.data(), data.size());
        if (written >= 0) {
            assert(data.size() >= written);
            remain = data.size() - written;
            if (remain == 0 && m_write_cb) {
                m_loop->RunInLoop([conn = shared_from_this()]() { std::invoke(conn->m_write_cb, conn); });
            }

        } else {
            int err = errno;
            written = 0;
            if (!(err == EAGAIN || err == EWOULDBLOCK || err == EINTR)) {
                LOG_ERROR(g_log) << "sendInLoop write faild errno=" << err << " " << strerror(err);
            }
            if (err == EPIPE || err == ECONNRESET) {
                handleError();
            }
        }
    }
    assert(remain <= data.size());
    if (remain > 0) {
        size_t old_len = m_output_buffer->ReadableSize();
        // 检查高水位
        if (old_len + remain >= m_hight_water_mark && old_len < m_hight_water_mark && m_hight_water_cb) {
            m_loop->RunInLoop(
                [conn = shared_from_this(), len = old_len + remain]() { conn->m_hight_water_cb(conn, len); });
        }
        // 写入到out buffer
        Slice remain_data(data.begin() + written, remain);
        m_output_buffer->Write(remain_data);
        if (!m_channel->isListeningWrite()) {
            m_channel->EnableWriteEvent();
        }
    }
}

socket_t TCPConnection::fd() const noexcept {
    return m_sock->fd();
}

void TCPConnection::setTcpNoDelay(bool on) {
    m_sock->setTcpNoDelay(on);
}