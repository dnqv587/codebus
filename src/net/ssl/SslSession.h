//***********************************************
// name      : SslSession.h
// brief     : 每连接 TLS 会话：SSL* + 内存 BIO 对（rbio=网络读入/wbio=网络写出）
//             + 非阻塞三态状态机（握手期上层明文排队、完成自动发）
// attention : 无 socket/loop 概念——U3 由 TCPConnection 驱动（feedCiphertext 进/
//             pullOutgoing 出）；状态查询方法（state/isEstablished/hasOutgoing/
//             handshakeElapsedNs/fatalWhat）跨线程安全（m_state 原子，U4 sweep 死线），
//             其余方法仍限单线程调用（ioLoop 线程）
//***********************************************
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "Buffer.hpp"
#include "Macro.h"
#include "net/ssl/SslContext.h"
#include "noncopyable.hpp"

namespace bus::ssl {

/// @brief 会话角色（决定 connect/accept 状态）
enum class Role {
    kClient,  ///< 客户端（主动发起 ClientHello）
    kServer   ///< 服务端（等待 ClientHello）
};

/// @brief TLS 会话状态（三态；独立于 TCPConnection 状态机——H4 决策 D-1）
enum class SslState {
    kHandshaking,  ///< 握手进行中（上层明文排队）
    kEstablished,  ///< 数据期
    kClosed        ///< 终态（fatal 或对端 close_notify；吸收一切后续调用）
};

/// @brief 非阻塞操作结果（SSL_get_error 五态映射）
enum class Want {
    kRead,        ///< 等待对端密文（喂入 feedCiphertext 后再驱）
    kWrite,       ///< 请先排空 outgoing 再驱（内存 BIO 下罕见）
    kEstablished, ///< 握手完成（内部已自动 flush 排队明文）
    kFatal,       ///< 协议/证书错误（fatalWhat() 留档；状态转 kClosed）
    kClosedPeer   ///< 对端 close_notify（状态转 kClosed）
};

/// @brief 每连接 TLS 会话（握手/加解密/优雅关闭）
class SslSession : noncopyable {
 public:
    using ptr = std::shared_ptr<SslSession>;

    /// @param role 客户端/服务端
    /// @param ctx 证书与安全基线来源（U1 工厂产物；生命周期须长于本对象）
    /// @attention 构造即设定 connect/accept 状态（零 IO）；client 首个 flight 由首次 advance() 产出
    SslSession(Role role, const SslContext& ctx);

    /// @brief 释放 SSL（连带 rbio/wbio——SSL_set_bio 已转移所有权）
    ~SslSession();

    /// @brief 推进握手（幂等可重入）；握手完成时自动 flush 排队明文入 wbio
    /// @return Want（见枚举注释）
    ATTR_NODISCARD
    Want advance();

    /// @brief 网络读侧入：socket 收到的密文喂入 rbio（握手期与数据期通用）
    void feedCiphertext(const std::byte* data, size_t size);

    /// @brief 解密：SSL_read 循环产出明文入内部明文缓冲（经 plaintext()/consumePlaintext 消费）
    /// @return Want：kRead=已榨干需更多密文；kClosedPeer=收到 close_notify；kFatal；握手期调用安全返回 kRead
    ATTR_NODISCARD
    Want decrypt();

    /// @brief 明文发送（所有权移交）：握手期排队（完成自动发）；数据期直接 SSL_write 入 wbio
    /// @param data 明文数据（本调用返回后即可复用/释放——内容已拷贝或已消费）
    /// @param size 字节数
    /// @return 接管字节数（= size；fatal/已关闭/已发 close_notify 返回 0）
    size_t encrypt(const std::byte* data, size_t size);

    /// @brief 取待发密文（wbio 全量搬入内部密文缓冲后返回可读视图）
    /// @return 密文视图（含握手 flight/密文记录/close_notify/alert）
    /// @attention 返回的 Slice 在下次调用本类任何方法前有效；发出 n 字节后调 skipOutgoing(n)
    ATTR_NODISCARD
    Slice pullOutgoing();

    /// @brief 确认已发出 n 字节密文（跳过内部密文缓冲读区；未发完的部分下次 pullOutgoing 仍可取）
    void skipOutgoing(size_t n) noexcept;

    /// @brief 优雅关闭第一段：发出 close_notify（密文随 pullOutgoing 取走；不等待对端回执）
    /// @attention 其后的 encrypt 返回 0；decrypt 仍可工作（接收对端回执转 kClosedPeer）
    void sendCloseNotify();

    /// @brief 设置对端主机名（SNI + 证书主机名校验；仅客户端角色）
    /// @param host 主机名或 IP 字符串（如 "localhost"/"127.0.0.1"）
    /// @attention 须在握手完成前调用（装配点：decorator 内 attach 后立即设置）；
    ///            IP 串走 set1_ip_asc（SAN IP 匹配），DNS 名走 set1_host；verify 关闭时
    ///            校验参数不生效（SNI 仍发送）
    void setPeerName(std::string_view host);

    // ---- 明文消费接口（U3 msg_cb 的载体，PeekAll/SkipRead 同构） ----

    /// @brief 已解密待消费明文（内部缓冲可读区视图）
    ATTR_NODISCARD
    Slice plaintext() noexcept {
        return m_plain_in.PeekAll();
    }
    /// @brief 消费 n 字节明文（上层 parser 已处理部分）
    void consumePlaintext(size_t n) noexcept {
        m_plain_in.SkipRead(n);
    }
    /// @brief 明文缓冲原生访问（TCPConnection::inputBuffer 的 TLS 重定向目标——
    ///        MessageCallback 的 Buffer* 签名载体，SkipRead/consume 语义复用）
    ATTR_NODISCARD
    Buffer* plainBuffer() noexcept {
        return &m_plain_in;
    }

    // ---- 查询（U3/U4 用） ----

    ATTR_PURE_INLINE
    SslState state() const noexcept {
        return m_state;
    }
    ATTR_PURE_INLINE
    bool isEstablished() const noexcept {
        return m_state == SslState::kEstablished;
    }
    ATTR_PURE_INLINE
    bool hasPendingPlaintext() const noexcept {
        return m_pending_plain.ReadableSize() > 0;
    }
    ATTR_PURE_INLINE
    size_t pendingPlaintextSize() const noexcept {
        return m_pending_plain.ReadableSize();
    }

    /// @brief 是否有待发密文（wbio 或内部密文缓冲任一非空——纯查询，不搬运）
    ATTR_NODISCARD
    bool hasOutgoing() const noexcept {
        return m_cipher_out.ReadableSize() > 0 || (m_wbio != nullptr && BIO_pending(m_wbio) > 0);
    }

    /// @brief 握手已耗时（ns；自构造起——U4 sweep 握手死线用）
    ATTR_NODISCARD
    uint64_t handshakeElapsedNs() const noexcept;

    /// @brief Fatal 错误描述（含 OpenSSL ERR 队列 dump；无错误为空串）
    ATTR_NODISCARD
    const std::string& fatalWhat() const noexcept {
        return m_fatal_what;
    }

 private:
    /// @brief SSL_get_error 错误分类（fatal 时 MarkFatal 留档并转 kClosed）
    Want MapSslError(int ret);
    /// @brief 排队明文批量入 wbio（WANT_WRITE 暂停等下次驱动）
    void FlushPendingPlaintext();
    /// @brief fatal 留档并关闭（ERR 队列拼入 m_fatal_what）
    void MarkFatal(const char* what);

    /// @brief 会话角色
    Role m_role;
    /// @brief OpenSSL 会话句柄（独占；SSL_free 连带释放两个 BIO）
    SSL* m_ssl;
    /// @brief 网络读侧 BIO（喂入密文；所有权随 SSL_set_bio 归 m_ssl）
    BIO* m_rbio;
    /// @brief 网络写侧 BIO（取出密文；所有权随 SSL_set_bio 归 m_ssl）
    BIO* m_wbio;
    /// @brief 会话状态（原子：base loop sweep 死线跨线程查询 vs ioLoop 写——U4）
    std::atomic<SslState> m_state;
    /// @brief 已发 close_notify（其后 encrypt 返 0；不改变状态机）
    bool m_shutdown_sent;
    /// @brief 对端关闭标志（kClosed 的成因区分：kClosedPeer vs kFatal）
    bool m_peer_closed;
    /// @brief 解密产出（上层消费载体）
    Buffer m_plain_in;
    /// @brief wbio 暂存（pullOutgoing/skipOutgoing 惯用法）
    Buffer m_cipher_out;
    /// @brief 握手期排队明文（完成自动 flush；WANT_WRITE 防御兜底）
    Buffer m_pending_plain;
    /// @brief fatal 错误描述（ERR 队列）
    std::string m_fatal_what;
    /// @brief 握手起始时刻（死线核算）
    std::chrono::steady_clock::time_point m_start;
};

}  // namespace bus::ssl
