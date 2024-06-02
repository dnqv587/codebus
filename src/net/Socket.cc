#include "Socket.h"
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <cassert>
#include <fcntl.h>
#include "log/Logger.h"
#include "exception/Exception.hpp"

namespace uf{

static Logger& g_logger = GET_LOGGER("system");

namespace detail{
static socket_t CreateSocket(Socket::Family domain, Socket::Type type)
{
	int realType{};
	switch(type)
	{
	case Socket::TCP:
		realType = SOCK_STREAM  | SOCK_CLOEXEC;
		break;
	case Socket::UDP:
		realType = SOCK_DGRAM  | SOCK_CLOEXEC;
		break;
	case Socket::NIO_TCP:
		realType = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
		break;
	case Socket::NIO_UDP:
		realType = SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
		break;
	}
	socket_t fd = ::socket(domain,realType,0);
	if(fd <= 0)
	{
		LOG_ERROR(g_logger)<<"create socket error,domain="<<domain<<" type="<<type;
        throw Exception::SocketError("create socket error");
	}
	return fd;
}

template<typename T>
bool  setSocketOption(int fd,int level,int option,const T& val)
{
    bool OK=true;
    if(::setsockopt(fd,level,option,&val,sizeof(T)) < 0)
    {
        OK = false;
        LOG_ERROR(g_logger)<<"set Socket Option error,fd="<<fd<<" option="<<option<<" err="<<strerror(errno);
    }
    return OK;
}
}

Socket::~Socket()
{
    assert(m_fd > 0);
    if(::close(m_fd) < 0)
    {
        LOG_ERROR(g_logger)<<"Socket close error,fd="<<m_fd;
    }
}

Socket::Socket(Socket::Family domain, Socket::Type type)
:m_fd(detail::CreateSocket(domain,type))
{
}
bool Socket::bind(const Address& addr) const noexcept
{
    assert(m_fd > 0);
    bool OK = true;
	if(::bind(m_fd,addr.getSockAddr(),addr.Length()) < 0)
    {
        OK = false;
        LOG_ERROR(g_logger)<<"Socket bind error,err="<<strerror(errno)
                    <<",fd="<<m_fd<<" address="<<addr.ipString()<<":"<<addr.port();
    }
    return OK;
}

bool Socket::listen() const noexcept
{
    assert(m_fd > 0);
    bool OK = true;
    if(::listen(m_fd,SOMAXCONN) < 0)
    {
        OK = false;
        LOG_ERROR(g_logger)<<"Socket listen error,err="<<strerror(errno)<<",fd="<<m_fd;
    }
    return OK;
}

Socket Socket::accept(Address& outAddr) const noexcept
{
    assert(m_fd > 0);
    int connfd{};
    do
    {
        connfd = ::accept(m_fd, outAddr.getSockAddr(), nullptr);
    } while (errno == EINTR);

    //FIXME:返回的socket设为nonblock和cloexec？

    if (connfd < 0)
    {
        LOG_ERROR(g_logger)<<"Socket accept error,err="<<strerror(errno);
    }
    return Socket(connfd);
}

bool Socket::connect(const Address &addr) const noexcept
{
    assert(m_fd > 0);
    bool OK = true;
    if(::connect(m_fd,addr.getSockAddr(),addr.Length()) < 0)
    {
        OK = false;
        LOG_ERROR(g_logger)<<"Socket connect error,fd="<<m_fd<<" address="<<addr.ipString()<<':'<<addr.port();
    }
    return OK;
}

bool Socket::close() const noexcept
{
    assert(m_fd > 0);
    bool OK = true;
    if(::close(m_fd) < 0)
    {
        OK = false;
        LOG_ERROR(g_logger)<<"Socket close error,fd="<<m_fd;
    }
    return OK;
}

bool Socket::ShutdownWrite() const noexcept
{
    bool OK = true;
    if(::shutdown(m_fd,SHUT_WR) < 0)
    {
        OK = false;
        LOG_ERROR(g_logger)<<"Socket ShutdownWrite error,fd="<<m_fd;
    }
    return OK;
}

bool Socket::setTcpNoDelay(bool on) const noexcept
{
    return detail::setSocketOption(m_fd,IPPROTO_TCP,TCP_NODELAY,on?1:0);
}

bool Socket::setKeepAlive(bool on) const noexcept
{
    return detail::setSocketOption(m_fd,SOL_SOCKET,SO_KEEPALIVE,on?1:0);
}

bool Socket::setReuseAddr(bool on) const noexcept
{
    return detail::setSocketOption(m_fd,SOL_SOCKET,SO_REUSEADDR,on?1:0);
}

bool Socket::setReuseExclusiveAddr(bool on) const noexcept
{
    return detail::setSocketOption(m_fd,SOL_SOCKET,SO_REUSEPORT,on?1:0);
}

int Socket::getSocketErrno() const noexcept
{
    int err{0};
    if(!detail::setSocketOption(m_fd,SOL_SOCKET,SO_REUSEADDR,err))
    {
        return errno;
    }
    return err;
}

bool Socket::setNonBlockAndCloseOnExec() const noexcept
{
    bool OK = true;
    int flag = fcntl(m_fd, F_GETFL);
    flag |= O_NONBLOCK | FD_CLOEXEC;
    if(fcntl(m_fd, F_SETFL, flag)<0)
    {
        OK = false;
        LOG_ERROR(g_logger)<<"set NonBlockAndCloseOnExec error,err="<<strerror(errno);
    }
    return OK;
}

std::optional<Address> Socket::getLocalAddr() const noexcept
{
    sockaddr_in6 addr{};
    socklen_t len = sizeof(sockaddr_in6);
    if(::getsockname(m_fd, sockaddr_cast<sockaddr>(&addr),&len) < 0)
    {
		LOG_ERROR(g_logger)<<"Socket getLocalAddr error,fd="<<m_fd;
		return std::nullopt;
    }

	switch (addr.sin6_family)
	{
	case AF_INET:
		return Address(*sockaddr_cast<sockaddr_in>(&addr));
	case AF_INET6:
		return Address(addr);
	default:
		return Address(*sockaddr_cast<sockaddr_in>(&addr));
	}
}

std::optional<Address> Socket::getRemoteAddr() const noexcept
{
    sockaddr_in6 addr{};
    socklen_t len = sizeof(sockaddr_in6);
    if(::getpeername(m_fd, sockaddr_cast<sockaddr>(&addr),&len) < 0)
    {
        LOG_ERROR(g_logger)<<"Socket getRemoteAddr error,fd="<<m_fd;
        return std::nullopt;
    }

    switch (addr.sin6_family)
    {
        case AF_INET:
            return Address(*sockaddr_cast<sockaddr_in>(&addr));
        case AF_INET6:
            return Address(addr);
        default:
            return Address(*sockaddr_cast<sockaddr_in>(&addr));
    }
}

size_t Socket::read(void *buf, size_t count) const noexcept
{
    return ::read(m_fd,buf,count);
}

size_t Socket::write(const void *buf, size_t count) const noexcept
{
    return ::write(m_fd,buf,count);
}

size_t Socket::readv(iovec *buf, int count) const noexcept
{
    return ::readv(m_fd,buf,count);
}

size_t Socket::writev(const iovec *buf, int count) const noexcept
{
    return ::writev(m_fd,buf,count);
}



}