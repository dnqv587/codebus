#include "TCPConnection.h"
#include <sys/sendfile.h>
#include "Channel.h"
#include "log/Logger.h"
#include "Macro.h"
#include "Socket.h"
#include "net/EventLoop.h"
#include "net/ssl/SslSession.h"

using namespace bus;

static Logger* g_log = GET_LOGGER("sys");

TCPConnection::TCPConnection(EventLoop* loop, std::string_view name, socket_t sock, const Address& localAddr,
                             const Address& remoteAddr)
    : m_loop(loop),
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

void TCPConnection::attachSsl(std::shared_ptr<ssl::SslSession> session) {
    // 装配点（NewConnectionDecorator）在 connectEstablished 之前调用——连接尚未启用
    // 任何事件，无并发窗口；established 后 attach 属装配错误：Debug 断言 + Release 拒绝（A1）
    assert(m_status == Connecting && "attachSsl must be called before connectEstablished");
    if (m_status != Connecting) {
        LOG_ERROR(g_log) << "TCPConnection [" << m_name << "] attachSsl rejected: status="
                         << StatusEnumToString(m_status) << " (expect Connecting)";
        return;
    }
    m_ssl = std::move(session);
}

bool TCPConnection::sslHandshaking() const noexcept {
    return m_ssl && !m_ssl->isEstablished();
}

bool TCPConnection::isSslFatal() const noexcept {
    // peer 优雅关闭（close_notify）fatalWhat 恒空——判据与 kClosedPeer 互斥
    return m_ssl && m_ssl->state() == ssl::SslState::kClosed && !m_ssl->fatalWhat().empty();
}

bool TCPConnection::isSslEstablished() const noexcept {
    return !m_ssl || m_ssl->isEstablished();
}

uint64_t TCPConnection::sslHandshakeElapsedNs() const noexcept {
    return m_ssl ? m_ssl->handshakeElapsedNs() : 0;
}

Buffer* TCPConnection::inputBuffer() noexcept {
    // TLS：明文缓冲承接（上层 SkipRead/consume 与单飞回灌语义零改动）
    return m_ssl ? m_ssl->plainBuffer() : m_input_buffer.get();
}

bool TCPConnection::sslDrained() const noexcept {
    return m_output_buffer->ReadableSize() == 0 && !m_ssl->hasPendingPlaintext() && !m_ssl->hasOutgoing();
}

void TCPConnection::sslFatal() {
    LOG_ERROR(g_log) << "TCPConnection [" << m_name << "] TLS fatal: " << m_ssl->fatalWhat();
    handleError();
}

void TCPConnection::handleRead() {
    assert(m_loop->isInLoopThread());
    ssize_t n = m_sock->read(m_input_buffer.get());
    if (n > 0) {
        if (m_ssl) {
            // TLS 读路径：密文即取即消费喂入会话（明文积压语义由 plainBuffer 承接，
            // 上层 inputBuffer()/msg_cb 消费语义与明文模式一致）
            Slice cipher = m_input_buffer->PeekAll();
            m_ssl->feedCiphertext(cipher.data(), cipher.size());
            m_input_buffer->SkipRead(cipher.size());
            ssl::Want w = m_ssl->advance();
            if (w == ssl::Want::kFatal) {
                sslFatal();
                return;
            }
            flushSslOutgoing();
            if (w == ssl::Want::kWrite) {
                // 罕见态（wbio 满）：flush 后再驱一轮（A4）
                w = m_ssl->advance();
                if (w == ssl::Want::kFatal) {
                    sslFatal();
                    return;
                }
                flushSslOutgoing();
            }
            if (m_ssl->isEstablished()) {
                ssl::Want rd = m_ssl->decrypt();
                if (rd == ssl::Want::kFatal) {
                    sslFatal();
                    return;
                }
                if (rd == ssl::Want::kClosedPeer) {
                    handleClose();  // 对端 close_notify（等价明文模式读 0）
                    return;
                }
                if (m_ssl->plainBuffer()->ReadableSize() > 0) {
                    std::invoke(m_msg_cb, shared_from_this(), m_ssl->plainBuffer());
                }
                // sendFile 在握手期/排队明文期的延迟触发恢复（sendFile 闭包跳过点）
                if (m_pending_file_fd >= 0 && m_output_buffer->ReadableSize() == 0 && sslDrained()) {
                    pumpPendingFileTls();
                }
            }
            return;  // 握手期无明文产出（死线由上层 sweep——U4）
        }
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
        // M2：无可写数据——但排队文件段待泵（U3：sendfile EAGAIN 后写事件直达此处）
        if (m_pending_file_fd >= 0) {
            pumpPendingFile();
            return;
        }
        // 排水完成与写事件就绪的竞态/伪唤醒——直接收尾，
        // 避免 write(fd,ptr,0)==0 返回 0 落入错误分支读残留 errno 误关连接
        m_channel->DisableWriteEvent();
        return;
    }
    ssize_t n = m_sock->write(buf.data(), buf.size());
    if (n > 0) {
        m_output_buffer->SkipRead(n);
        if (m_output_buffer->ReadableSize() == 0) {
            // TLS：排空后先排会话（排队明文可产新密文——A6）；仅剩握手期排队明文时
            // 无可写内容，关写事件防自旋（读事件驱动握手，完成点 flush 接管）
            if (m_ssl) {
                flushSslOutgoing();
                if (!sslDrained()) {
                    if (m_output_buffer->ReadableSize() == 0) {
                        m_channel->DisableWriteEvent();
                    }
                    return;
                }
            }
            // 排队文件段接续（U3：缓冲空后 sendfile 零拷贝段——完成后自行触发 write_cb）
            if (m_pending_file_fd >= 0) {
                if (m_ssl) {
                    pumpPendingFileTls();  // TLS 降级链（pread+encrypt）
                } else {
                    pumpPendingFile();
                }
                return;
            }
            m_channel->DisableWriteEvent();
            // 半关闭挂起：shutdown 置 Disconnecting 后数据才排空（TLS 含 close_notify），此时关写方向
            if (m_status == Disconnecting) {
                m_sock->ShutdownWrite();
            }
            if (m_write_cb) {
                m_loop->QueueInLoop([conn = shared_from_this()]() { std::invoke(conn->m_write_cb, conn); });
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
    if (m_ssl) {
        // TLS：尽力补发 close_notify——established=首发；对端先关（peer-closed）=回执
        // 己方（双向优雅关闭常规序列）；fatal/已发过=sendCloseNotify 内部守卫自会跳过。
        // best-effort 直写，吞掉一切错误（A2：EPIPE 等再走 handleError 会无限重入）
        m_ssl->sendCloseNotify();
        for (;;) {
            Slice out = m_ssl->pullOutgoing();
            if (out.size() == 0) {
                break;
            }
            ssize_t wn = m_sock->write(out.data(), out.size());
            if (wn <= 0) {
                break;
            }
            m_ssl->skipOutgoing(static_cast<size_t>(wn));
        }
    }
    m_channel->DisableAllEvent();
    // 终态须在回调之前写入（D38）：conn_cb 观察断开，close_cb 可能销毁本对象
    m_status = Disconnected;
    if (m_conn_cb) {
        std::invoke(m_conn_cb, shared_from_this());  // Disconnected 通知（muduo 对齐）：
    }  // 客户端与服务端用户均可观察连接关闭
    if (m_close_cb) {
        std::invoke(m_close_cb, shared_from_this());  // 最后一步（D38：其后零成员访问）
    }
    // 关闭观察者（多播；close_cb 之后——观察者仅状态清理，销毁竞态不在此处理）
    for (const auto& ob : m_close_observers) {
        if (ob) {
            ob();
        }
    }
    m_close_observers.clear();
    m_context.reset();  // 上下文随连接终结（防外部经 shared_ptr 续命观察者悬挂）
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
    if (m_ssl) {
        // TLS：驱动首个 flight（client 产 ClientHello；server 等读）——conn_cb 时序
        // 与明文完全一致（D-1：观察者只见 TCP 建立）；首驱 fatal（如 ctx 异常）经
        // handleError→handleClose 收尾（conn_cb 已在彼处触发，此处直接返回防双发）
        if (m_ssl->advance() == ssl::Want::kFatal) {
            sslFatal();
            return;
        }
        flushSslOutgoing();
    }
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
    if (m_ssl) {
        // TLS：先发 close_notify（密文随发尽后由 drained 门控关写方向）
        m_ssl->sendCloseNotify();
        flushSslOutgoing();
    }
    // U3：排队文件段也算待发数据（sendfile 途中关写=截断流）；pump 完毕分支补关；
    // TLS：close_notify 密文与排队明文同样计入待发
    if (!m_channel->isListeningWrite() && m_output_buffer->ReadableSize() == 0 &&
        m_pending_file_fd < 0 && (!m_ssl || sslDrained())) {
        m_sock->ShutdownWrite();  // 无待发数据：立即关写方向
    }
    // 否则：handleWrite 排空输出缓冲后补关（见 handleWrite 排空分支）
}

void TCPConnection::send(const std::string_view s) {
    queueSendData(reinterpret_cast<const std::byte*>(s.data()),s.size());
}

void TCPConnection::send(const std::byte* data, size_t len) {
    queueSendData(data, len);
}

void TCPConnection::send(Slice msg) {
    queueSendData(msg.data(),msg.size());
}

void TCPConnection::send(Buffer* buf) {
    if (m_status != Connected || buf == nullptr) {
        return;
    }
    if (m_loop->isInLoopThread()) {
        // loop 线程：同步消费可读区（无生命周期窗口）
        Slice readable = buf->PeekAll();
        sendInLoop(readable.data(), readable.size());
        buf->SkipRead(readable.size());
    } else {
        // 非 loop 线程：立即拷贝数据入队并消费——调用方的 Buffer 在 send
        // 返回后即可安全复用/销毁（数据所有权转移语义）
        Slice readable = buf->PeekAll();
        queueSendData(readable.data(), readable.size());
        buf->SkipRead(readable.size());
    }
}

void TCPConnection::sendFile(int fd, uint64_t offset, uint64_t count) {
    m_loop->RunInLoop([this, conn = shared_from_this(), fd, offset, count]() {
        m_pending_file_fd = fd;
        m_pending_file_off = offset;
        m_pending_file_left = count;
        if (m_output_buffer->ReadableSize() == 0 && (!m_ssl || sslDrained())) {
            if (m_ssl) {
                pumpPendingFileTls();  // TLS 降级链（pread+encrypt——sendfile 仅明文通道）
            } else {
                pumpPendingFile();  // 缓冲已空（头未排队或已排空）——直接泵
            }
        }
        // 否则：handleWrite 排空缓冲后接续（见 handleWrite 收尾分支）；
        // TLS 握手期的排队明文另经 handleRead 完成点恢复（见 handleRead TLS 分支尾）
    });
}

bool TCPConnection::flushSslOutgoing() {
    assert(m_loop->isInLoopThread());
    for (;;) {
        Slice out = m_ssl->pullOutgoing();
        if (out.size() == 0) {
            break;
        }
        ssize_t n = m_sock->write(out.data(), out.size());
        if (n > 0) {
            m_ssl->skipOutgoing(static_cast<size_t>(n));
            if (static_cast<size_t>(n) == out.size()) {
                continue;  // 本批写完，取下一批
            }
        }
        int err = errno;
        if (n < 0 && !(err == EAGAIN || err == EWOULDBLOCK || err == EINTR)) {
            handleError();  // 真错误（EPIPE 等）
            return false;
        }
        // 部分写/EAGAIN：剩余密文入输出缓冲（写事件重驱；密文计入高水位——R2 对策，
        // 公式与 sendInLoop 明文路径同款）
        size_t written = n > 0 ? static_cast<size_t>(n) : 0;
        size_t remain = out.size() - written;
        size_t old_len = m_output_buffer->ReadableSize();
        if (old_len + remain >= m_hight_water_mark && old_len < m_hight_water_mark && m_hight_water_cb) {
            m_loop->RunInLoop(
                [conn = shared_from_this(), len = old_len + remain]() { conn->m_hight_water_cb(conn, len); });
        }
        m_output_buffer->Write(out.data() + written, remain);
        m_ssl->skipOutgoing(remain);
        if (!m_channel->isListeningWrite()) {
            m_channel->EnableWriteEvent();
        }
        break;
    }
    return m_output_buffer->ReadableSize() == 0 && !m_ssl->hasPendingPlaintext();
}

/// @brief TLS sendfile 降级推进（loop 线程；背压界=256KB/会话未排空，handleWrite 续驱）
void TCPConnection::pumpPendingFileTls() {
    assert(m_loop->isInLoopThread());
    while (m_pending_file_left > 0) {
        // A3 背压界：输出缓冲或会话未排空即停（含握手期 encrypt 排队场景）
        if (m_output_buffer->ReadableSize() > 256_KB || !sslDrained()) {
            m_channel->EnableWriteEvent();  // 写事件重驱（handleWrite 排空后接续）
            return;
        }
        std::byte buf[64_KB];
        size_t want = static_cast<size_t>(std::min<uint64_t>(m_pending_file_left, sizeof(buf)));
        ssize_t r = ::pread(m_pending_file_fd, buf, want, static_cast<off_t>(m_pending_file_off));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR(g_log) << "TLS sendfile pread fd=" << m_pending_file_fd << " ret=" << r
                             << " err=" << strerror(errno);
            m_pending_file_fd = -1;  // 出错：放弃文件段（上层 write_cb/关闭链接管）
            return;
        }
        if (r == 0) {
            LOG_ERROR(g_log) << "TLS sendfile pread hit EOF early fd=" << m_pending_file_fd
                             << " left=" << m_pending_file_left;
            m_pending_file_fd = -1;
            return;
        }
        m_ssl->encrypt(buf, static_cast<size_t>(r));
        m_pending_file_off += static_cast<uint64_t>(r);
        m_pending_file_left -= static_cast<uint64_t>(r);
        flushSslOutgoing();
    }
    m_pending_file_fd = -1;  // 完毕（收尾与 pumpPendingFile 同构）
    if (m_output_buffer->ReadableSize() == 0 && sslDrained()) {
        m_channel->DisableWriteEvent();
        if (m_status == Disconnecting) {
            m_sock->ShutdownWrite();
        }
        if (m_write_cb) {
            m_loop->QueueInLoop([conn = shared_from_this()]() { std::invoke(conn->m_write_cb, conn); });
        }
    }
}

/// @brief sendfile 推进（loop 线程；EAGAIN 保写事件；完毕触发 write_cb 路径）
void TCPConnection::pumpPendingFile() {
    while (m_pending_file_left > 0) {
        ssize_t n = ::sendfile(m_sock->fd(), m_pending_file_fd,
                               reinterpret_cast<off_t*>(&m_pending_file_off),
                               static_cast<size_t>(std::min<uint64_t>(m_pending_file_left, 1_MB)));
        if (n > 0) {
            m_pending_file_left -= static_cast<uint64_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            m_channel->EnableWriteEvent();  // 写事件重驱
            return;
        }
        LOG_ERROR(g_log) << "sendfile fd=" << m_pending_file_fd << " ret=" << n
                         << " err=" << strerror(errno);
        m_pending_file_fd = -1;  // 出错：放弃文件段（上层 write_cb/关闭链接管）
        return;
    }
    m_pending_file_fd = -1;  // 完毕
    if (m_output_buffer->ReadableSize() == 0) {
        m_channel->DisableWriteEvent();
        if (m_status == Disconnecting) {
            m_sock->ShutdownWrite();
        }
        if (m_write_cb) {
            m_loop->QueueInLoop([conn = shared_from_this()]() { std::invoke(conn->m_write_cb, conn); });
        }
    }
}

void TCPConnection::queueSendData(const std::byte* data, size_t size) {
    if (m_status != Connected) {
        // 停机/断开窗口内丢弃留痕（DEBUG 级防刷屏；I1 不静默——U1/Q36）
        LOG_DEBUG(g_log) << "TCPConnection [" << m_name << "] send dropped on non-connected";
        return;
    }
    // 拷贝数据内容后排入 loop（D9）：调用方缓冲区在 send 返回后无生命周期约束——
    // 闭包只可持内容拷贝，捕裸指针会在 loop 线程执行时悬垂（UAF）
    std::vector<std::byte> owned(data, data + size);
    m_loop->RunInLoop([conn = shared_from_this(), owned = std::move(owned)]() {
        conn->sendInLoop(owned.data(), owned.size());
    });
}

void TCPConnection::sendInLoop(const std::byte* data,size_t size) {
    assert(m_loop->isInLoopThread());
    if (m_status == Disconnected) {
        LOG_WARN(g_log) << "disconnected, give up writing";
        return;
    }
    if (m_ssl) {
        // TLS 写路径：明文所有权移交会话（握手期排队，established 直接入 wbio）——
        // 密文经 flushSslOutgoing 直写/EAGAIN 排队（高水位语义对密文同样生效）
        size_t taken = m_ssl->encrypt(data, size);
        if (taken == 0) {
            LOG_WARN(g_log) << "TCPConnection [" << m_name
                            << "] TLS send dropped (session closed or close_notify sent)";
            return;
        }
        if (flushSslOutgoing() && !m_ssl->hasPendingPlaintext() && m_write_cb) {
            m_loop->QueueInLoop([conn = shared_from_this()]() { std::invoke(conn->m_write_cb, conn); });
        }
        return;
    }
    ssize_t written = 0;
    size_t remain = size;
    // channel未监听写事件，且output buffer无数据。直接写
    if (!m_channel->isListeningWrite() && m_output_buffer->ReadableSize() == 0) {
        written = m_sock->write(data, size);
        if (written >= 0) {
            assert(size >= written);
            remain = size - written;
            if (remain == 0 && m_write_cb) {
                m_loop->QueueInLoop([conn = shared_from_this()]() { std::invoke(conn->m_write_cb, conn); });
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
    assert(remain <= size);
    if (remain > 0) {
        size_t old_len = m_output_buffer->ReadableSize();
        // 检查高水位
        if (old_len + remain >= m_hight_water_mark && old_len < m_hight_water_mark && m_hight_water_cb) {
            m_loop->RunInLoop(
                [conn = shared_from_this(), len = old_len + remain]() { conn->m_hight_water_cb(conn, len); });
        }
        // 写入到out buffer
        m_output_buffer->Write(data+written,remain);
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