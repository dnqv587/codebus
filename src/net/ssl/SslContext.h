//***********************************************
// name      : SslContext.h
// brief     : SSL_CTX 的 RAII 工厂与安全基线（服务端证书链/客户端对端校验）
//             + 进程级 OpenSSL 初始化
// attention : 每个 Server/Client 装配点持有一个（进程内共享语义）；
//             线程安全由 OpenSSL 内部锁保证；SslSession 经 raw() 取原生句柄构造
//***********************************************
#pragma once
#include <memory>
#include <string_view>

#include <openssl/ssl.h>

#include "Macro.h"
#include "noncopyable.hpp"

namespace bus::ssl {

/// @brief SSL_CTX RAII 持有者；工厂函数装配安全基线，失败抛 Exception::SslError
class SslContext : noncopyable {
 public:
    using ptr = std::shared_ptr<SslContext>;

    /// @brief 服务端上下文：加载证书链与私钥（TLS_server_method）
    /// @param certChainFile PEM 证书链文件路径
    /// @param privateKeyFile PEM 私钥文件路径
    /// @return 装配完成的上下文
    /// @throw Exception::SslError 文件缺失/解析失败/证书私钥不配对（what 含 ERR 队列 dump）
    ATTR_NODISCARD
    static ptr createServer(std::string_view certChainFile, std::string_view privateKeyFile);

    /// @brief 客户端上下文：默认开启对端校验（SSL_VERIFY_PEER + 系统默认 CA 路径）
    /// @return 装配完成的上下文
    /// @attention caFile/verifyPeer 定制在 HttpClient 装配层（H4-U5）经 useCaFile/disableVerify 扩展
    ATTR_NODISCARD
    static ptr createClient();

    /// @brief 追加 CA 信任锚（客户端）
    /// @param caFile PEM CA 文件路径；空串 = 使用系统默认 CA 路径
    /// @throw Exception::SslError 加载失败
    void useCaFile(std::string_view caFile);

    /// @brief 关闭对端校验（仅测试/内网受控场景；WARN 日志留痕）
    void disableVerify() noexcept;

    /// @brief 原生句柄（SslSession 构造用；生命周期归本对象）
    ATTR_NODISCARD
    SSL_CTX* raw() const noexcept {
        return m_ctx.get();
    }

 private:
    explicit SslContext(SSL_CTX* ctx) noexcept;

    struct CtxDeleter {
        void operator()(SSL_CTX* c) const noexcept {
            SSL_CTX_free(c);
        }
    };
    /// @brief OpenSSL 上下文（RAII 独占）
    std::unique_ptr<SSL_CTX, CtxDeleter> m_ctx;
};

/// @brief 进程级 OpenSSL 初始化（幂等；各工厂入口自动调用）
void ensureOpenSslInit() noexcept;

}  // namespace bus::ssl
