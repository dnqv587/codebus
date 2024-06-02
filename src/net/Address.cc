#include "Address.h"
#include <endian.h>
#include <arpa/inet.h>
#include <cstring>
#include <cerrno>
#include <cassert>
#include "log/Logger.h"
#include "exception/Exception.hpp"

namespace uf
{

static Logger& g_logger = GET_LOGGER("system");

namespace detail{

static sockaddr_in ParseIPv4(std::string_view strAddr, uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_port=htobe16(port);
	if(strAddr == "*")
	{
		addr.sin_addr.s_addr=INADDR_ANY;
	}
	else if(::inet_pton(AF_INET,strAddr.data(),&addr.sin_addr)<0)
    {
        LOG_ERROR(g_logger)<< "Address Parse IPv4 error, Addr="<<strAddr<<" port="<<port<<",err="<<strerror(errno);
        throw Exception::AddressError("Address Parse error");
    }
    return addr;
}

static sockaddr_in6 ParseIPv6(std::string_view strAddr, uint16_t port)
{
    sockaddr_in6 addr{};
    addr.sin6_family=AF_INET6;
    addr.sin6_port=htobe16(port);
	if(strAddr == "*")
	{
		addr.sin6_addr = in6addr_any;
	}
	else if(::inet_pton(AF_INET6,strAddr.data(),&addr.sin6_addr)<0)
    {
        LOG_ERROR(g_logger)<< "Address Parse IPv6 error, Addr="<<strAddr<<" port="<<port<<",err="<<strerror(errno);
        throw Exception::AddressError("Address Parse error");
    }
    return addr;
}

static std::string ToFormatIP(const sockaddr_in6& addr )
{
	std::string ip;
	ip.resize(46);
	if(::inet_ntop(AF_INET6,&addr.sin6_addr,ip.data(),ip.size()) == nullptr)
	{
		LOG_WARN(g_logger)<< "Address ToFormatIP IPv4 error err="<<strerror(errno);
	}
    return ip;
}

static std::string ToFormatIP(const sockaddr_in& addr )
{
	std::string ip;
	ip.resize(16);
	if(::inet_ntop(AF_INET,&addr.sin_addr.s_addr,ip.data(),ip.size()) == nullptr)
	{
		LOG_WARN(g_logger)<< "Address ToFormatIP IPv4 error, Addr="<<addr.sin_addr.s_addr<<"err="<<strerror(errno);
	}
    return ip;
}

ATTR_PURE_INLINE
static int16_t getPort(const sockaddr_in& addr)
{
     return be16toh(addr.sin_port);
}
ATTR_PURE_INLINE
static int16_t getPort(const sockaddr_in6& addr)
{
    return be16toh(addr.sin6_port);
}
ATTR_PURE_INLINE
static sa_family_t getFamily(const sockaddr_in& addr)
{
    return addr.sin_family;
}

ATTR_PURE_INLINE
static sa_family_t getFamily(const sockaddr_in6& addr)
{
    return addr.sin6_family;
}

}

Address::Address(std::string_view addr, uint16_t port, bool ipv6)
{
	assert(port>0 && port<65535);
	if(ipv6)
	{
		assert(std::count(addr.begin(), addr.end(),'.') == 4);
		m_addr = detail::ParseIPv6(addr,port);
	}
	else
	{
		assert(addr.find(':')!=std::string_view::npos);
		m_addr = detail::ParseIPv4(addr,port);
	}
}
std::string Address::ipString() const
{
	return std::visit([](const auto& ip){
		return detail::ToFormatIP(ip);
	},m_addr);
}

uint16_t Address::port() const
{
    return std::visit([](const auto& port){
        return detail::getPort(port);
    },m_addr);
}

sa_family_t Address::family() const noexcept
{
    return std::visit([](const auto& port){
        return detail::getFamily(port);
    },m_addr);
}
const sockaddr* Address::getSockAddr() const
{
    return std::visit([]( auto& addr)-> auto {
        return sockaddr_cast<const sockaddr>(&addr);
    },m_addr);
}

sockaddr *Address::getSockAddr()
{
    return std::visit([]( auto& addr)-> auto {
        return sockaddr_cast<sockaddr>(&addr);
    },m_addr);
}

 size_t Address::Length() const
{
    return std::visit([](const auto& addr)-> size_t {
        return sizeof(addr);
    },m_addr);
}

}