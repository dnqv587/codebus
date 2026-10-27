#pragma once
#include <netinet/in.h>
#include <variant>
#include <string_view>
#include "Macro.h"
#include "type.h"

namespace bus {

class Address {
 public:
    using SockAddr = std::variant<sockaddr_in, sockaddr_in6>;
    using family_t = sa_family_t;
    using ref = std::reference_wrapper<Address>;

    Address() = default;

    explicit Address(SockAddr&& addr) noexcept : m_addr(std::forward<SockAddr>(addr)) {}

    /// @brief 从 sockaddr_in 构造 IPv4 地址
    explicit Address(sockaddr_in&& addr) noexcept : m_addr(std::forward<sockaddr_in>(addr)) {}

    /// @brief 从 sockaddr_in6 构造 IPv6 地址
    explicit Address(sockaddr_in6&& addr) noexcept : m_addr(std::forward<sockaddr_in6>(addr)) {}

    Address(std::string_view addr, uint16_t port, bool ipv6 = false);

    /// @brief 从 "ip:port"（IPv6 用 "[x::y]:port"）字符串解析构造
    /// @param addr 地址串
    /// @return 解析后的地址
    /// @throw Exception::AddressError 格式非法或 IP 不可解析
    static Address FromIpPortString(std::string_view addr);

    ATTR_PURE_INLINE
    const sockaddr_in6* getSockAddrInet6() const { return &std::get<sockaddr_in6>(m_addr); }
    ATTR_PURE_INLINE
    const sockaddr_in* getSockAddrInet4() const { return &std::get<sockaddr_in>(m_addr); }
    ATTR_NODISCARD
    const sockaddr* getSockAddr() const;
    ATTR_NODISCARD
    sockaddr* getSockAddr();
    ATTR_NODISCARD
    size_t Length() const;

    ATTR_NODISCARD
    std::string ipString() const;
    ATTR_NODISCARD
    uint16_t port() const;
    std::string toString() const;
    ATTR_NODISCARD
    family_t family() const noexcept;
    ATTR_NODISCARD
    bool isIPV6() const noexcept;

 private:
    std::variant<sockaddr_in, sockaddr_in6> m_addr;
};

template <typename To, typename From,
          std::enable_if_t<
              is_one_of<std::decay_t<To>, sockaddr_in, sockaddr_in6, sockaddr> && !std::is_same_v<From, To>, int> = 0>
ATTR_PURE_INLINE
To* sockaddr_cast(From* f) {
    return static_cast<To*>(implicit_cast<std::conditional_t<std::is_const_v<To>, const void*, void*>>(f));
}

}  // namespace bus
