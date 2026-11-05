//***********************************************
// name      : SslContext.cc
// brief     : SSL_CTX 工厂实现：进程级初始化、安全基线、证书装配
// attention : None
//***********************************************
#include "net/ssl/SslContext.h"

#include <mutex>
#include <string>

#include <openssl/err.h>

#include "Exception.hpp"
#include "log/Logger.h"

namespace bus::ssl {

namespace {

Logger* g_logger = GET_LOGGER("sys");

/// @brief 拼装 OpenSSL ERR 队列（≤5 条）到错误串（拼完顺带清空队列）
std::string DumpErrQueue(std::string_view what) {
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

/// @brief 抛 SslError（what + ERR 队列）——异常路径统一出口
[[noreturn]] void ThrowSsl(std::string_view what) {
    throw Exception::SslError(DumpErrQueue(what).c_str());
}

/// @brief 公共安全基线（服务端/客户端同款）
///        TLS1.2 下限（R3 重协商/旧协议面收窄）；禁重协商；禁压缩；
///        部分写语义（U2 SslSession 的 SSL_write 重试依赖，R1）
void ApplyBaseline(SSL_CTX* ctx) {
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);
}

}  // namespace

void ensureOpenSslInit() noexcept {
    static std::once_flag flag;
    std::call_once(flag, [] {
        // 幂等初始化 + 错误串装载（SslError 的 ERR 队列可读性依赖）
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    });
}

SslContext::SslContext(SSL_CTX* ctx) noexcept : m_ctx(ctx) {}

SslContext::ptr SslContext::createServer(std::string_view certChainFile, std::string_view privateKeyFile) {
    ensureOpenSslInit();

    // guard 持有：装配中途抛异常时 RAII 释放半成品 ctx
    std::unique_ptr<SSL_CTX, CtxDeleter> guard(SSL_CTX_new(TLS_server_method()));
    if (!guard) {
        ThrowSsl("SslContext: SSL_CTX_new(server) failed");
    }
    ApplyBaseline(guard.get());
    // TLS1.3 会话票据关闭：回环测试避免跨用例会话复用干扰握手路径覆盖
    SSL_CTX_set_num_tickets(guard.get(), 0);

    const std::string cert(certChainFile);
    const std::string key(privateKeyFile);
    if (SSL_CTX_use_certificate_chain_file(guard.get(), cert.c_str()) != 1) {
        ThrowSsl("SslContext: load certificate chain failed: " + cert);
    }
    if (SSL_CTX_use_PrivateKey_file(guard.get(), key.c_str(), SSL_FILETYPE_PEM) != 1) {
        ThrowSsl("SslContext: load private key failed: " + key);
    }
    if (SSL_CTX_check_private_key(guard.get()) != 1) {
        ThrowSsl("SslContext: certificate and private key do not match");
    }
    return ptr(new SslContext(guard.release()));
}

SslContext::ptr SslContext::createClient() {
    ensureOpenSslInit();

    std::unique_ptr<SSL_CTX, CtxDeleter> guard(SSL_CTX_new(TLS_client_method()));
    if (!guard) {
        ThrowSsl("SslContext: SSL_CTX_new(client) failed");
    }
    ApplyBaseline(guard.get());
    SSL_CTX_set_verify(guard.get(), SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_default_verify_paths(guard.get()) != 1) {
        ThrowSsl("SslContext: load system default CA paths failed");
    }
    return ptr(new SslContext(guard.release()));
}

void SslContext::useCaFile(std::string_view caFile) {
    if (caFile.empty()) {
        if (SSL_CTX_set_default_verify_paths(m_ctx.get()) != 1) {
            ThrowSsl("SslContext: load system default CA paths failed");
        }
        return;
    }
    const std::string ca(caFile);
    if (SSL_CTX_load_verify_locations(m_ctx.get(), ca.c_str(), nullptr) != 1) {
        ThrowSsl("SslContext: load CA file failed: " + ca);
    }
}

void SslContext::disableVerify() noexcept {
    LOG_WARN(g_logger) << "SslContext: peer verification DISABLED (untrusted/controlled use only)";
    SSL_CTX_set_verify(m_ctx.get(), SSL_VERIFY_NONE, nullptr);
}

}  // namespace bus::ssl
