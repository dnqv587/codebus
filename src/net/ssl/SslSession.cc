//***********************************************
// name      : SslSession.cc
// brief     : TLS 会话实现：握手驱动/加解密/优雅关闭（内存 BIO 非阻塞模型）
// attention : None
//***********************************************
#include "net/ssl/SslSession.h"

#include <algorithm>

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>

#include "type.h"

namespace bus::ssl {

namespace {

/// @brief 单次 BIO/SSL 操作的搬运上限（防巨型单调用）
constexpr size_t kChunkLimit = 1_MB;
/// @brief pullOutgoing 的栈缓冲
constexpr size_t kPullChunk = 16_KB;
/// @brief decrypt 的栈缓冲
constexpr size_t kReadChunk = 64_KB;

/// @brief 拼装 OpenSSL ERR 队列（≤5 条；拼完顺带清空队列）
std::string DumpErrQueue(const char* what) {
    std::string msg(what);
    char buf[256];
    unsigned long err = 0;
    int count = 0;
    while ((err = ERR_get_error()) != 0 && count < 5) {
        ERR_error_string_n(err, buf, sizeof(buf));
        msg += " | ";
        msg += buf;
        ++count;
    }
    return msg;
}

}  // namespace

SslSession::SslSession(Role role, const SslContext& ctx)
    : m_role(role),
      m_ssl(nullptr),
      m_rbio(nullptr),
      m_wbio(nullptr),
      m_state(SslState::kHandshaking),
      m_shutdown_sent(false),
      m_peer_closed(false),
      m_plain_in(4_KB),
      m_cipher_out(4_KB),
      m_pending_plain(1_KB),
      m_start(std::chrono::steady_clock::now()) {
    ensureOpenSslInit();
    m_ssl = SSL_new(ctx.raw());
    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    if (m_ssl == nullptr || m_rbio == nullptr || m_wbio == nullptr) {
        // 半成品回收：set_bio 尚未执行，BIO 所有权还在本对象
        BIO_free(m_rbio);
        BIO_free(m_wbio);
        m_rbio = nullptr;
        m_wbio = nullptr;
        MarkFatal("SslSession: SSL_new/BIO_new failed");
        return;
    }
    SSL_set_bio(m_ssl, m_rbio, m_wbio);  // 所有权转移：SSL_free 连带释放
    if (role == Role::kClient) {
        SSL_set_connect_state(m_ssl);
    } else {
        SSL_set_accept_state(m_ssl);
    }
}

SslSession::~SslSession() {
    if (m_ssl != nullptr) {
        SSL_free(m_ssl);  // 连带释放 rbio/wbio
    }
}

Want SslSession::advance() {
    if (m_ssl == nullptr) {
        return Want::kFatal;  // 构造失败已留档
    }
    if (m_state == SslState::kClosed) {
        return m_peer_closed ? Want::kClosedPeer : Want::kFatal;
    }
    if (m_state == SslState::kEstablished) {
        return Want::kEstablished;
    }
    ERR_clear_error();
    int ret = SSL_do_handshake(m_ssl);
    if (ret == 1) {
        m_state = SslState::kEstablished;
        FlushPendingPlaintext();  // D-1：握手期排队明文自动发出
        return Want::kEstablished;
    }
    return MapSslError(ret);
}

void SslSession::feedCiphertext(const std::byte* data, size_t size) {
    if (m_state == SslState::kClosed || m_rbio == nullptr) {
        return;  // kClosed 吸收
    }
    size_t off = 0;
    while (off < size) {
        size_t chunk = std::min(size - off, kChunkLimit);
        int n = BIO_write(m_rbio, data + off, static_cast<int>(chunk));
        if (n <= 0) {
            MarkFatal("SslSession: BIO_write(rbio) failed");
            return;
        }
        off += static_cast<size_t>(n);
    }
}

Want SslSession::decrypt() {
    if (m_ssl == nullptr) {
        return Want::kFatal;
    }
    if (m_state == SslState::kClosed) {
        return m_peer_closed ? Want::kClosedPeer : Want::kFatal;
    }
    if (m_state != SslState::kEstablished) {
        return Want::kRead;  // 握手期调用安全（无明文可产）
    }
    std::byte buf[kReadChunk];
    for (;;) {
        ERR_clear_error();
        int n = SSL_read(m_ssl, buf, sizeof(buf));
        if (n > 0) {
            m_plain_in.Write(buf, static_cast<size_t>(n));
            continue;  // 连续榨干（TLS record 可能多帧堆叠）
        }
        // n<=0：ZERO_RETURN=close_notify；WANT_READ=榨干；其余 fatal
        return MapSslError(n);
    }
}

size_t SslSession::encrypt(const std::byte* data, size_t size) {
    if (m_ssl == nullptr || m_state == SslState::kClosed || m_shutdown_sent) {
        return 0;  // kClosed/已关写方向：不接管
    }
    if (size == 0) {
        return 0;
    }
    if (m_state != SslState::kEstablished || m_pending_plain.ReadableSize() > 0) {
        // 握手未完成或队列未清（保序）：排队，完成点/驱动点自动 flush
        m_pending_plain.Write(data, size);
        FlushPendingPlaintext();
        return size;
    }
    size_t off = 0;
    while (off < size) {
        size_t chunk = std::min(size - off, kChunkLimit);
        ERR_clear_error();
        int n = SSL_write(m_ssl, data + off, static_cast<int>(chunk));
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        int err = SSL_get_error(m_ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            // 内存 BIO 下罕见（防御兜底）：未完成部分入队
            m_pending_plain.Write(data + off, size - off);
            return size;
        }
        MapSslError(n);  // fatal/ZERO_RETURN——状态已转 kClosed
        return 0;
    }
    return size;
}

Slice SslSession::pullOutgoing() {
    if (m_wbio == nullptr) {
        return m_cipher_out.PeekAll();
    }
    while (BIO_pending(m_wbio) > 0) {
        std::byte buf[kPullChunk];
        int n = BIO_read(m_wbio, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        m_cipher_out.Write(buf, static_cast<size_t>(n));
    }
    return m_cipher_out.PeekAll();
}

void SslSession::skipOutgoing(size_t n) noexcept {
    m_cipher_out.SkipRead(n);
}

void SslSession::sendCloseNotify() {
    // 已收到对端 close_notify（kClosed 且 m_peer_closed）时仍须回执自己的——
    // 双向优雅关闭的常规序列；仅 fatal 终态/已发过时不再发
    if (m_ssl == nullptr || m_shutdown_sent || (m_state == SslState::kClosed && !m_peer_closed)) {
        return;
    }
    m_shutdown_sent = true;
    ERR_clear_error();
    (void)SSL_shutdown(m_ssl);  // 首段：close_notify 已入 wbio；0/-1 均可（对端回执不死等——死线兜底）
}

void SslSession::setPeerName(std::string_view host) {
    if (m_ssl == nullptr || m_role != Role::kClient) {
        return;  // 仅客户端角色（服务端 SNI 回调出轮——H5+）
    }
    const std::string name(host);
    // SNI 扩展（服务端按名选证书/路由的前提）
    (void)SSL_set_tlsext_host_name(m_ssl, name.c_str());
    // 证书主机名校验：IP 与 DNS 名走不同匹配器（set1_host 不做 IP 匹配）
    in6_addr addr{};
    if (::inet_pton(AF_INET, name.c_str(), &addr) == 1 || ::inet_pton(AF_INET6, name.c_str(), &addr) == 1) {
        (void)X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(m_ssl), name.c_str());
    } else {
        (void)SSL_set1_host(m_ssl, name.c_str());
    }
}

uint64_t SslSession::handshakeElapsedNs() const noexcept {
    auto elapsed = std::chrono::steady_clock::now() - m_start;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

Want SslSession::MapSslError(int ret) {
    int err = SSL_get_error(m_ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:
            return Want::kRead;
        case SSL_ERROR_WANT_WRITE:
            return Want::kWrite;
        case SSL_ERROR_ZERO_RETURN:
            m_peer_closed = true;
            m_state = SslState::kClosed;
            return Want::kClosedPeer;
        case SSL_ERROR_SYSCALL:
            if (ERR_peek_error() == 0) {
                // 无 OpenSSL 错误的 SYSCALL = 对端未经 close_notify 的 EOF
                m_peer_closed = true;
                m_state = SslState::kClosed;
                return Want::kClosedPeer;
            }
            MarkFatal("SslSession: SSL syscall error");
            return Want::kFatal;
        default:
            MarkFatal("SslSession: SSL fatal error");
            return Want::kFatal;
    }
}

void SslSession::FlushPendingPlaintext() {
    if (m_state != SslState::kEstablished || m_ssl == nullptr) {
        return;
    }
    while (m_pending_plain.ReadableSize() > 0) {
        Slice front = m_pending_plain.PeekAll();
        size_t chunk = std::min(front.size(), kChunkLimit);
        ERR_clear_error();
        int n = SSL_write(m_ssl, front.data(), static_cast<int>(chunk));
        if (n > 0) {
            m_pending_plain.SkipRead(static_cast<size_t>(n));
            continue;
        }
        int err = SSL_get_error(m_ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            return;  // wbio 待排空后由下次驱动续写
        }
        MapSslError(n);
        return;
    }
}

void SslSession::MarkFatal(const char* what) {
    m_fatal_what = DumpErrQueue(what);
    m_state = SslState::kClosed;
}

}  // namespace bus::ssl
