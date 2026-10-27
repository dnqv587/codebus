//***********************************************
// name      : TcpClient.h
// brief     : TCP 客户端：Connector 非阻塞连接（含 retry 退避）+ 生命周期编排
// attention : Connector 的 fd 为原生 socket_t（成功后移交 TCPConnection，
//             不经 Socket 封装——Socket 无所有权移交出口且其析构契约
//             禁止 close 后再析构）；退避定时器经自持 TimerManager（仅
//             启用 retry 时惰性创建）；析构前必须 Stop 完成分派轮外收尾
//***********************************************
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "Timer.h"
#include "Macro.h"
#include "noncopyable.hpp"
#include "type.h"
#include "view.h"
#include "net/Address.h"
#include "net/TCPConnection.h"

namespace bus {

class EventLoop;
class Channel;

//***********************************************
// name      : Connector
// brief     : 非阻塞连接器：connect 三档分流 + SO_ERROR 判定 + retry 退避
// attention : Start/Stop 可跨线程调用（内部投递 loop，闭包持 shared_ptr
//             保活）；Restart 须在 loop 线程；成功后 fd 所有权移交上层
//             （NewConnectionCallback）；失败且未启用 retry 时经
//             FailedCallback 通知（本库统一失败语义，超出 muduo）；
//             **必须经 make_shared/shared_ptr 创建**（Start/Stop/retry 依赖
//             shared_from_this，栈对象调用将抛 bad_weak_ptr）
//***********************************************
class Connector : noncopyable, public std::enable_shared_from_this<Connector> {
 public:
    using ptr = std::shared_ptr<Connector>;
    /// @brief 连接成功（fd 所有权移交回调方）
    using NewConnectionCallback = std::function<void(socket_t)>;
    /// @brief 连接失败且未启用 retry（统一失败通知入口；
    ///        fd 所有权移交回调方——用于构造短命连接，由其 RAII 关闭）
    using FailedCallback = std::function<void(socket_t)>;

    Connector(View<EventLoop> loop, const Address& serverAddr);

    /// @brief 析构（契约：必须已 Stop——fd 已移交/关闭、定时器宿主已销毁）
    ~Connector();

    /// @brief 发起连接（首次调用有效；重复调用被忽略并记警告）
    /// @attention 未启用 retry 的连接器为**单次生命周期**：连接失败或关闭后
    ///            再次 Start 无效——需重连须重建，或启用 enableRetry
    void Start();

    /// @brief 重置状态与退避间隔并立即发起新连接（断线重连入口；loop 线程）
    void Restart();

    /// @brief 停止：取消重试定时器、关闭在途连接（可跨线程调用）
    void Stop();

    void setNewConnectionCallback(NewConnectionCallback cb) noexcept {
        m_new_conn_cb = std::move(cb);
    }

    void setFailedCallback(FailedCallback cb) noexcept {
        m_failed_cb = std::move(cb);
    }

    /// @brief 启用失败退避重连（须在 Start 前调用）
    void enableRetry() noexcept {
        m_retry = true;
    }

    /// @brief 是否启用重连
    ATTR_PURE_INLINE
    bool retryEnabled() const noexcept {
        return m_retry;
    }

 private:
    /// @brief 连接状态
    enum State { kDisconnected = 0, kConnecting = 1, kConnected = 2 };

    /// @brief 连接主体（loop 线程）：建 socket → connect 三档分流
    void startInLoop();

    /// @brief 挂写事件等待连接完成（含立即成功路径，统一经 SO_ERROR 判定）
    /// @param sockfd 连接 fd
    void connecting(socket_t sockfd);

    /// @brief 写事件就绪：摘事件后经 SO_ERROR 判定成败（loop 线程）
    void handleWrite();

    /// @brief 错误事件：按连接失败处理（loop 线程）
    void handleError();

    /// @brief 退避重试（loop 线程；经惰性 TimerManager 定时到点再 startInLoop）
    void retryInLoop();

    /// @brief Stop 主体（loop 线程）：取消定时器、关闭在途连接
    void stopInLoop();

    /// @brief 失败/重试/移交后的善后：关闭 fd 并回到 kDisconnected
    /// @param reason 日志语境
    void closeInFlightSocket(const char* reason) noexcept;

    /// @brief 移交在途 fd（不关闭、状态回 kDisconnected；!retry 失败通知用）
    socket_t handoverInFlightSocket() noexcept;

    /// @brief 临时错误善后：!retry 且有失败回调 → fd 移交通知；否则关闭后重试
    /// @param reason 日志语境
    void failOrRetry(const char* reason);

    /// @brief 致命错误善后（不重试）：!retry 且有失败回调 → fd 移交通知；否则关闭
    /// @param reason 日志语境
    void failNoRetry(const char* reason);

    View<EventLoop> m_loop;
    /// @brief 服务器地址
    const Address m_server_addr;
    /// @brief 连接状态（跨线程读）
    std::atomic<int> m_state{kDisconnected};
    /// @brief 是否已 Start 且未 Stop（停止后定时器与在途路径据此让路）
    std::atomic<bool> m_connect{false};
    /// @brief 是否启用失败重连（仅 loop 线程读写）
    bool m_retry{false};
    /// @brief 当前退避间隔[ms]（kInitRetryDelayMs 起 ×2 至 kMaxRetryDelayMs 封顶）
    int m_retry_delay_ms{kInitRetryDelayMs};
    /// @brief 连接 fd（未在途/已移交为 -1；移交后永不再触碰）
    socket_t m_fd{-1};
    /// @brief 连接期事件通道（失败/重试后由下次 connecting 重建）
    std::unique_ptr<Channel> m_channel;
    /// @brief 重试定时器宿主（仅首次 Retry 时创建；Stop 时在 loop 线程析构）
    std::unique_ptr<TimerManager> m_timer_manager;
    /// @brief 待触发/已触发的重试定时器
    Timer::ptr m_retry_timer;
    NewConnectionCallback m_new_conn_cb;
    FailedCallback m_failed_cb;

    static constexpr int kInitRetryDelayMs = 500;
    static constexpr int kMaxRetryDelayMs = 30 * 1000;
};

//***********************************************
// name      : TcpClient
// brief     : TCP 客户端：Connector 连接 + TCPConnection 生命周期编排
// attention : connect 异步（结果经 conn_cb：Connected=建立；Disconnected=
//             失败(!retry)或关闭）；析构前必须 stop() 且连接已断；stop 后
//             不可再 connect（单向生命周期，对称 TcpServer）；**回调内只允许
//             stop()**——销毁须在收到 Disconnected 后于 loop 线程外进行
//             （违约在 release 下 LOG_FATAL+Abort）
//***********************************************
class TcpClient : noncopyable {
 public:
    /// @brief 构造 TCP 客户端（不发起连接；先 set 回调再 connect）
    /// @param loop 所属事件循环（单 loop：连接与连接器共用）
    /// @param server_addr 服务器地址 "ip:port"（IPv6 用 "[x::y]:port"）
    /// @param name 客户端名（连接名前缀）
    /// @throw Exception::AddressError 地址格式非法
    TcpClient(EventLoop* loop, std::string_view server_addr, std::string name = "TcpClient");

    /// @brief 析构（契约：已 stop 且连接已断——conn_cb 收到过 Disconnected；
    ///        否则 close_cb 捕获的 this 将悬垂）
    ~TcpClient();

    /// @brief 发起连接（异步；结果经 conn_cb 通知；幂等；stop 后拒绝）
    /// @attention 非 retry 客户端为单连接生命周期：连接断开后再次 connect
    ///            无效（Connector 侧记警告）；断线自动重连须 enableRetry
    void connect();

    /// @brief 断开：已连接则优雅半关（shutdown，已排队数据发完后对端收 EOF）
    void disconnect();

    /// @brief 停止连接尝试与重试（幂等；已建立的连接不受影响）
    void stop();

    /// @brief 启用失败退避重连（须在 connect 前调用）
    void enableRetry() noexcept {
        m_connector->enableRetry();
    }

    /// @brief 当前连接（未建立/已断开为空；跨线程安全）
    ATTR_NODISCARD
    TCPConnection::ptr connection() const noexcept {
        return m_connection.load(std::memory_order_acquire);
    }

    /// @brief 连接建立/断开/失败通知回调（loop 线程；失败时收到 Disconnected
    ///        态的短命连接对象，send 会被状态机安全拒绝）
    void setConnectionCallback(const ConnectionCallback& cb) noexcept {
        m_conn_cb = cb;
    }

    /// @brief 消息到达回调（loop 线程）
    void setMessageCallback(const MessageCallback& cb) noexcept {
        m_msg_cb = cb;
    }

    /// @brief 数据全部写出回调
    void setWriteCompleteCallback(const TCPConnection::WriteCompleteCallback& cb) noexcept {
        m_write_cb = cb;
    }

    /// @brief 发送缓冲高水位回调
    void setHighWaterMarkCallback(const TCPConnection::HighWaterMarkCallback& cb, size_t mark) noexcept {
        m_high_water_cb = cb;
        m_high_water_mark = mark;
    }

 private:
    /// @brief 连接成功（Connector 回调，loop 线程）：建连并进入 Connected
    void newConnection(socket_t sockfd);

    /// @brief 连接失败且未启用 retry（Connector 回调，loop 线程）：
    ///        短命连接统一通知（conn_cb 收 Disconnected）
    void failedConnection(socket_t fd);

    /// @brief 连接关闭（close_cb，loop 线程）：清指针，按需自动重连
    void removeConnection(const TCPConnection::ptr& conn);

    /// @brief 所属事件循环
    EventLoop* m_loop;
    /// @brief 服务器地址（已解析）
    const Address m_server_addr;
    /// @brief 客户端名（连接名前缀）
    const std::string m_name;
    /// @brief 连接器（shared：loop 侧闭包保活，析构发生在分派轮外收尾后）
    std::shared_ptr<Connector> m_connector;
    /// @brief 当前连接（C++20 原子共享指针：loop 写、任意线程读）
    std::atomic<std::shared_ptr<TCPConnection>> m_connection;
    /// @brief 是否已 stop（单向生命周期：stop 后拒绝再 connect）
    std::atomic<bool> m_stopped{false};
    /// @brief 连接编号发生器
    std::atomic<uint32_t> m_next_conn_id{1};

    ConnectionCallback m_conn_cb;
    MessageCallback m_msg_cb;
    TCPConnection::WriteCompleteCallback m_write_cb;
    TCPConnection::HighWaterMarkCallback m_high_water_cb;
    size_t m_high_water_mark = 64_MB;
};

}  // namespace bus
