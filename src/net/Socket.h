#pragma once
#include <sys/uio.h>
#include <optional>
#include "noncopyable.hpp"
#include "type.h"
#include "Macro.h"
#include "Address.h"
#include <utility>
#include <memory>

namespace bus{

//***********************************************
// name      : Socket
// brief     : RAII封装socket文件描述符，拥有整个socket的生命周期
// attention : None
//***********************************************
class Buffer;
class Socket:noncopyable
{
public:
	enum Family
	{
		IPV4 = AF_INET,
		IPV6 = AF_INET6,
		UNIX = AF_UNIX
	};

	enum Type
	{
		TCP,
		UDP,
		NIO_TCP,
		NIO_UDP
	};
    using ptr = std::shared_ptr<Socket>;

    static constexpr socket_t INVALID_SOCKET = -1;

    explicit Socket (const socket_t fd) noexcept :m_fd(fd){}
	Socket(Family domain,Type type);
    ~Socket();

    ATTR_PURE_INLINE
    socket_t fd() const noexcept
    {return m_fd;}
    /// @brief 绑定地址和端口
    /// @param addr 地址信息
	bool bind(const Address& addr) const noexcept;
    /// @brief 监听socket
    /// @note backlog为SOMAXCONN，可修改系统配置net.core.somaxconn
    bool listen() const noexcept;
    /// @brief 接受连接请求
    /// @param outAddr [out]连接的地址
    /// @return Socket对象
    std::pair<socket_t,Address> accept() const noexcept;
    /// @brief 向addr发起连接请求
    /// @param addr 地址和端口
    bool connect(const Address& addr) const noexcept;
    /// @brief 关闭socket，并释放资源
    bool close() noexcept;

    /// @brief 分散读缓冲区，将buf读取到分散的内存块中
    /// @param buf 内存块
    /// @param count 长度
    /// @return 读取的字节数，发生错误返回-1
    ssize_t readv(iovec* buf,int count) const noexcept;
    ssize_t read(void* buf,size_t count) const noexcept;
    ssize_t read(Buffer* buf) const noexcept;

    /// @brief 集中写缓冲区，将分散的数据块写入到统一缓冲区中
    /// @param buf 数据块
    /// @param count 长度
    /// @return 写入的字节数，发生错误返回-1
    ssize_t writev(const iovec* buf,int count) const noexcept;
    ssize_t write(const void* buf,size_t count) const noexcept;
    /// @brief 关闭写通道
    /// @return
    bool ShutdownWrite() const noexcept;
    /// @brief 禁用Nagle算法，避免连续发包出现延迟
    /// @param on 是否启用
    bool setTcpNoDelay(bool on) const noexcept;
    /// @brief 发送周期性保活报文以维持连接
    /// @note 不足以代替应用层的心跳，其心跳周期太长和只能判定对端的TCP连接是否正常无法判断其他异常情况
    bool setKeepAlive(bool on) const noexcept;
    /// @brief 设置端口复用
    bool setReuseAddr(bool on) const noexcept;
    /// @brief 设置端口复用
    /// @pre 要求绑定同一ip/port的socket都设置为SO_REUSEPORT
    bool setReuseExclusiveAddr(bool on) const noexcept;
    /// @brief 设置socket为非阻塞和不被子进程继承
    bool setNonBlockAndCloseOnExec() const noexcept;
    /// @brief 获取socket的errno
    /// @return errno
    ATTR_NODISCARD
    int getSocketErrno() const noexcept;
    /// @brief 获取本地地址
    ATTR_NODISCARD
    std::optional<Address> getLocalAddr() const noexcept;
    /// @brief 获取远端地址
    ATTR_NODISCARD
    std::optional<Address> getRemoteAddr() const noexcept;
    ATTR_NODISCARD
    bool isAvailable() const noexcept{
        return m_fd > 0;
    }

private:
    socket_t m_fd;
};

}