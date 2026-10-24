//***********************************************
// name      : TcpServer.h
// brief     : TCP 服务器：Listener 接受连接，按 round-robin 分发到 IO 线程池，
//             管理连接生命周期（建立/回派/移除）与优雅停止
// attention : Start 幂等；Stop 全停（摘监听→逐连接关闭→停 IO 线程池→
//             DoneCallback 触发一次）；析构前必须 Stop 且等待 DoneCallback
//             （连接表须为空）；"ip:port" 支持 IPv4 与 "[IPv6]:port"
//***********************************************
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/Macro.h"
#include "base/noncopyable.hpp"
#include "base/type.h"
#include "net/Address.h"
#include "net/EventLoopThread.h"
#include "net/TCPConnection.h"

namespace bus
{

class EventLoop;
class Listener;

class TcpServer : noncopyable
{
 public:
    using DoneCallback = std::function<void ()>;

    /// @brief 构造 TCP 服务器（完成地址解析与端口绑定；先 set 回调再 Start）
    /// @param loop 主 loop（acceptor 所在）
    /// @param addr 监听地址，"ip:port"（IPv6 用 "[x::y]:port"）
    /// @param thread_num IO 线程数（0 = 单 loop 模式）
    /// @throw Exception::AddressError 地址格式非法或 IP 不可解析
    /// @throw Exception::SocketError 端口绑定失败（如端口被占用）
    TcpServer(EventLoop* loop, std::string_view addr, uint32_t thread_num);

    /// @brief 析构（须已 Stop 且 DoneCallback 已触发：连接表空、监听已摘）
    ~TcpServer();

    /// @brief 启动监听与 IO 线程池（幂等；重复调用无效果）
    /// @attention 生命周期单向：Stop 之后不可重启（Start 将被忽略并记警告）
    void Start();

    /// @brief 全停：摘监听 → 逐连接关闭 → 停 IO 线程池
    /// @param cb 完成回调（全部连接移除后在 base loop 内触发一次）
    /// @attention 重复 Stop 被忽略（首个回调生效）
    void Stop(DoneCallback cb = DoneCallback());

    /// @brief 连接建立/断开回调（ioLoop 线程内触发）
    void setConnectionCallback(const ConnectionCallback& cb) noexcept
    {
        m_conn_cb = cb;
    }

    /// @brief 消息到达回调（ioLoop 线程内触发）
    void setMessageCallback(const MessageCallback& cb) noexcept
    {
        m_msg_cb = cb;
    }

    /// @brief 数据全部写出回调
    void setWriteCompleteCallback(const TCPConnection::WriteCompleteCallback& cb) noexcept
    {
        m_write_cb = cb;
    }

    /// @brief 发送缓冲高水位回调
    void setHighWaterMarkCallback(const TCPConnection::HighWaterMarkCallback& cb,
                                  size_t mark) noexcept
    {
        m_high_water_cb = cb;
        m_high_water_mark = mark;
    }

    /// @brief 各 IO 线程初始化回调（loop 线程内、Loop() 之前）
    void setThreadInitCallback(const EventLoopThreadPool::ThreadInitCallback& cb) noexcept
    {
        m_thread_init_cb = cb;
    }

    /// @brief 当前连接数
    /// @attention 须在 base loop 线程调用（连接表仅 base loop 访问）
    ATTR_PURE_INLINE
    size_t ConnectionCount() const noexcept
    {
        return m_conn_map.size();
    }

 private:
    /// @brief 启动主体（base loop 线程）：装配 Listener 与 IO 线程池
    void startInLoop();

    /// @brief 新连接（base loop 线程，Listener 回调）：选 ioLoop、建连入表
    void handleNewConnection(socket_t sockfd, Address peer_addr);

    /// @brief 连接关闭（conn 的 ioLoop 线程，close_cb）：转交 base loop 移除
    void removeConnection(const TCPConnection::ptr& conn);

    /// @brief 连接出表（base loop 线程）：表空且停机中则收尾
    void removeConnectionInLoop(const TCPConnection::ptr& conn);

    /// @brief 全停主体（base loop 线程）
    void stopInLoop(DoneCallback cb);

    /// @brief 停机收尾（base loop 线程）：停线程池 + DoneCallback 触发一次
    void finishStop();

    /// @brief 主 loop（acceptor 所在；单 loop 模式兼作 IO loop）
    EventLoop* m_loop;
    /// @brief 监听地址（已解析）
    const Address m_listen_addr;
    /// @brief 服务器名（连接名前缀）
    const std::string m_name;
    /// @brief IO 线程数（0 = 单 loop）
    const uint32_t m_thread_num;

    ConnectionCallback m_conn_cb;
    MessageCallback m_msg_cb;
    TCPConnection::WriteCompleteCallback m_write_cb;
    TCPConnection::HighWaterMarkCallback m_high_water_cb;
    size_t m_high_water_mark = 64_MB;
    EventLoopThreadPool::ThreadInitCallback m_thread_init_cb;
    DoneCallback m_done_cb;

    /// @brief IO 线程池（空 = 单 loop 模式）
    std::unique_ptr<EventLoopThreadPool> m_pool;
    /// @brief 监听器（空 = 未启动或已停）
    std::unique_ptr<Listener> m_listener;
    /// @brief 连接表（仅 base loop 线程访问；key: 连接名）
    std::unordered_map<std::string, TCPConnection::ptr> m_conn_map;

    /// @brief 是否已启动（幂等标志）
    std::atomic<bool> m_started{false};
    /// @brief 连接编号发生器
    std::atomic<uint32_t> m_next_conn_id{1};
    /// @brief 停机中（仅 base loop 线程）
    bool m_stopping{false};
    /// @brief 完成回调已触发（仅 base loop 线程）
    bool m_done_invoked{false};
    /// @brief 停机收尾已完成（仅 base loop 线程；M3 幂等守卫）
    bool m_finished{false};
};

} // namespace bus
