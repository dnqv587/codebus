#include "Socket.h"
#include "log/Logger.h"
#include "Exception.hpp"
#include "Buffer.hpp"
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <cassert>
#include <fcntl.h>
#include <array>

namespace bus {

static Logger* g_logger = GET_LOGGER("sys");

namespace detail {
static socket_t CreateSocket(Socket::Family domain, Socket::Type type) {
    int realType{};
    switch (type) {
        case Socket::TCP:
            realType = SOCK_STREAM | SOCK_CLOEXEC;
            break;
        case Socket::UDP:
            realType = SOCK_DGRAM | SOCK_CLOEXEC;
            break;
        case Socket::NIO_TCP:
            realType = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
            break;
        case Socket::NIO_UDP:
            realType = SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
            break;
    }
    socket_t fd = ::socket(domain, realType, 0);
    if (fd <= 0) {
        LOG_ERROR(g_logger) << "create socket error,domain=" << domain << " type=" << type;
        // throw Exception::SocketError("create socket error");
        return Socket::INVALID_SOCKET;
    }
    return fd;
}

template <typename T>
bool setSocketOption(int fd, int level, int option, const T& val) {
    bool OK = true;
    if (::setsockopt(fd, level, option, &val, sizeof(T)) < 0) {
        OK = false;
        LOG_ERROR(g_logger) << "set Socket Option error,fd=" << fd << " option=" << option
                            << " err=" << strerror(errno);
    }
    return OK;
}
}  // namespace detail

Socket::~Socket() {
    assert(m_fd > 0);
    if (::close(m_fd) < 0) {
        LOG_ERROR(g_logger) << "Socket close error,fd=" << m_fd;
    }
}

Socket::Socket(Socket::Family domain, Socket::Type type) : m_fd(detail::CreateSocket(domain, type)) {}
bool Socket::bind(const Address& addr) const noexcept {
    assert(m_fd > 0);
    bool OK = true;
    if (::bind(m_fd, addr.getSockAddr(), addr.Length()) < 0) {
        OK = false;
        LOG_ERROR(g_logger) << "Socket bind error,err=" << strerror(errno) << ",fd=" << m_fd
                            << " address=" << addr.ipString() << ":" << addr.port();
    }
    return OK;
}

bool Socket::listen() const noexcept {
    assert(m_fd > 0);
    bool OK = true;
    if (::listen(m_fd, SOMAXCONN) < 0) {
        OK = false;
        LOG_ERROR(g_logger) << "Socket listen error,err=" << strerror(errno) << ",fd=" << m_fd;
    }
    return OK;
}

std::pair<socket_t, Address> Socket::accept() const noexcept {
    assert(m_fd > 0);
    int connfd{-1};
    sockaddr_in6 addr{};
    socklen_t addrlen = sizeof(addr);

    do {
        connfd = ::accept4(m_fd, sockaddr_cast<sockaddr>(&addr), &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (connfd < 0 && errno == EINTR);  // I7：仅失败时检查 errno——成功后
                                             // 残留 EINTR 会二次 accept 覆盖已得 fd

    if (connfd < 0) {
        // EAGAIN/EWOULDBLOCK：非阻塞 listen fd 积压排空的正常返回
        // （Listener 的 accept 循环依赖），不按错误记录
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR(g_logger) << "Socket accept error, err=" << strerror(errno);
        }
        return std::make_pair(connfd, Address());
    }

    // 根据 sin6_family 判断是 IPv4 还是 IPv6
    switch (addr.sin6_family) {
        case AF_INET:
            return std::make_pair(connfd, Address(*sockaddr_cast<sockaddr_in>(&addr)));
        case AF_INET6:
            return std::make_pair(connfd, Address(std::move(addr)));
        default:
            LOG_ERROR(g_logger) << "Socket accept unsupported address family: " << addr.sin6_family;
            ::close(connfd);
            return std::make_pair(INVALID_SOCKET, Address());
    }
}

bool Socket::connect(const Address& addr) const noexcept {
    assert(m_fd > 0);
    bool OK = true;
    if (::connect(m_fd, addr.getSockAddr(), addr.Length()) < 0) {
        OK = false;
        LOG_ERROR(g_logger) << "Socket connect error,fd=" << m_fd << " address=" << addr.ipString() << ':'
                            << addr.port();
    }
    return OK;
}

bool Socket::close() noexcept {
    assert(m_fd > 0);
    bool OK = true;
    if (::close(m_fd) < 0) {
        OK = false;
        LOG_ERROR(g_logger) << "Socket close error,fd=" << m_fd;
    }
    m_fd = INVALID_SOCKET;
    return OK;
}

bool Socket::ShutdownWrite() const noexcept {
    bool OK = true;
    if (::shutdown(m_fd, SHUT_WR) < 0) {
        OK = false;
        LOG_ERROR(g_logger) << "Socket ShutdownWrite error,fd=" << m_fd;
    }
    return OK;
}

std::pair<ssize_t, Address> Socket::recvFrom(void* buf, size_t count) const noexcept {
    assert(m_fd > 0);
    sockaddr_in6 addr{};
    socklen_t addrlen = sizeof(addr);
    const ssize_t n = ::recvfrom(m_fd, buf, count, MSG_DONTWAIT, sockaddr_cast<sockaddr>(&addr), &addrlen);
    if (n < 0) {
        return {n, Address()};  // EAGAIN/EINTR 等由调用方分类（排空循环）
    }
    // family 判定同 accept 模式（内核按实际 family 布局 sockaddr_in/6）
    if (addr.sin6_family == AF_INET6) {
        return {n, Address(std::move(addr))};
    }
    return {n, Address(std::move(*sockaddr_cast<sockaddr_in>(&addr)))};
}

bool Socket::sendTo(const void* buf, size_t count, const Address& addr) const noexcept {
    assert(m_fd > 0);
    // MSG_DONTWAIT：任意线程安全（阻塞 fd 也不会挂起调用线程）
    return ::sendto(m_fd, buf, count, MSG_DONTWAIT, addr.getSockAddr(), addr.Length()) == static_cast<ssize_t>(count);
}

bool Socket::setTcpNoDelay(bool on) const noexcept {
    return detail::setSocketOption(m_fd, IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0);
}

bool Socket::setKeepAlive(bool on) const noexcept {
    return detail::setSocketOption(m_fd, SOL_SOCKET, SO_KEEPALIVE, on ? 1 : 0);
}

bool Socket::setReuseAddr(bool on) const noexcept {
    return detail::setSocketOption(m_fd, SOL_SOCKET, SO_REUSEADDR, on ? 1 : 0);
}

bool Socket::setReusePort(bool on) const noexcept {
#ifdef SO_REUSEPORT
    return detail::setSocketOption(m_fd, SOL_SOCKET, SO_REUSEPORT, on ? 1 : 0);
#else
    return false;  // 平台无 SO_REUSEPORT（M5：原 #elif 无表达式，该分支编译失败）
#endif
}

bool Socket::setBroadcast(bool on) const noexcept {
    return detail::setSocketOption(m_fd, SOL_SOCKET, SO_BROADCAST, on ? 1 : 0);
}

bool Socket::joinGroup(const Address& multicastAddr) const noexcept {
    if (multicastAddr.isIPV6()) {
        LOG_ERROR(g_logger) << "Socket joinGroup: IPv6 multicast not supported yet";
        return false;
    }
    ip_mreq mreq{};
    const auto* sin = sockaddr_cast<const sockaddr_in>(multicastAddr.getSockAddr());
    mreq.imr_multiaddr = sin->sin_addr;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return ::setsockopt(m_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
}

bool Socket::leaveGroup(const Address& multicastAddr) const noexcept {
    if (multicastAddr.isIPV6()) {
        LOG_ERROR(g_logger) << "Socket leaveGroup: IPv6 multicast not supported yet";
        return false;
    }
    ip_mreq mreq{};
    const auto* sin = sockaddr_cast<const sockaddr_in>(multicastAddr.getSockAddr());
    mreq.imr_multiaddr = sin->sin_addr;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return ::setsockopt(m_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
}

bool Socket::setMulticastTTL(int ttl) const noexcept {
    return detail::setSocketOption(m_fd, IPPROTO_IP, IP_MULTICAST_TTL, ttl);
}

int Socket::getSocketErrno() const noexcept {
    int err{0};
    socklen_t len = sizeof(err);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
    return err;
}

bool Socket::setNonBlockAndCloseOnExec() const noexcept {
    bool OK = true;
    int flag = fcntl(m_fd, F_GETFL);
    flag |= O_NONBLOCK;
    if (fcntl(m_fd, F_SETFL, flag) < 0) {
        OK = false;
        LOG_ERROR(g_logger) << "set NonBlockAndCloseOnExec error,err=" << strerror(errno);
    }
    int fdflag = fcntl(m_fd, F_GETFD);
    if (fcntl(m_fd, F_SETFD, fdflag | FD_CLOEXEC) < 0) {
        OK = false;
        LOG_ERROR(g_logger) << "set NonBlockAndCloseOnExec error,err=" << strerror(errno);
    }
    return OK;
}

std::optional<Address> Socket::getLocalAddr() const noexcept {
    sockaddr_in6 addr{};
    socklen_t len = sizeof(sockaddr_in6);
    if (::getsockname(m_fd, sockaddr_cast<sockaddr>(&addr), &len) < 0) {
        LOG_ERROR(g_logger) << "Socket getLocalAddr error,fd=" << m_fd;
        return std::nullopt;
    }

    switch (addr.sin6_family) {
        case AF_INET:
            return Address(*sockaddr_cast<sockaddr_in>(&addr));
        case AF_INET6:
            return Address(std::move(addr));
        default:
            return Address(*sockaddr_cast<sockaddr_in>(&addr));
    }
}

std::optional<Address> Socket::getRemoteAddr() const noexcept {
    sockaddr_in6 addr{};
    socklen_t len = sizeof(sockaddr_in6);
    if (::getpeername(m_fd, sockaddr_cast<sockaddr>(&addr), &len) < 0) {
        LOG_ERROR(g_logger) << "Socket getRemoteAddr error,fd=" << m_fd;
        return std::nullopt;
    }

    switch (addr.sin6_family) {
        case AF_INET:
            return Address(*sockaddr_cast<sockaddr_in>(&addr));
        case AF_INET6:
            return Address(addr);
        default:
            return Address(*sockaddr_cast<sockaddr_in>(&addr));
    }
}

ssize_t Socket::read(void* buf, size_t count) const noexcept { return ::read(m_fd, buf, count); }

ssize_t Socket::write(const void* buf, size_t count) const noexcept { return ::write(m_fd, buf, count); }

ssize_t Socket::readv(iovec* buf, int count) const noexcept { return ::readv(m_fd, buf, count); }

ssize_t Socket::writev(const iovec* buf, int count) const noexcept { return ::writev(m_fd, buf, count); }

ssize_t Socket::read(Buffer* buf) const noexcept {
    // char extra_buf[65536];
    std::array<std::byte, 65536> extra_buf;
    struct iovec vec[2];
    const size_t writable = buf->WritableSize();
    vec[0].iov_base = buf->WriteBegin().base();
    vec[0].iov_len = writable;
    vec[1].iov_base = extra_buf.data();
    vec[1].iov_len = extra_buf.max_size();
    const int iovcnt = (writable < extra_buf.max_size()) ? 2 : 1;
    size_t n = readv(vec, iovcnt);
    if (n < 0) {
        LOG_ERROR(g_logger) << "Socket read to buffer error,fd=" << m_fd << " err:" << strerror(getSocketErrno());
    } else if (n <= writable) {
        buf->SkipWrite(n);
    } else {
        buf->SkipWrite(writable);
        Slice s(extra_buf.begin(), n - writable);
        buf->Write(s);
    }
    return n;
}

}  // namespace bus