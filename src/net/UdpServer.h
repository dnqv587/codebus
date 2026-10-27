//***********************************************
// name      : UdpServer.h
// brief     : UDP 服务端（同步模型）：接收主线程阻塞收包 + 任务线程池分发处理
// attention : 无 EventLoop 依赖；**创建与启动分离**（W1.1 用户裁决）——构造
//             期 bind 并创建接收线程对象与任务线程池（就绪不运行），Start()
//             才启动；MessageCallback 在工作者线程执行（非接收线程）；跨
//             数据报仅 worker_num=1 时保序（FIFO 队列 + 单工作者），并行
//             处理需保序请用应用层序号；禁止在 MessageCallback 内析构本
//             对象（Stop 将 join 工作者线程——死锁），**亦禁止在回调内调用
//             Stop()**（工作者 self-join：pthread_join 得 EDEADLK 后异常
//             逃逸致 terminate——停服请置原子标志由外部线程执行）；多实例
//             同址绑定时各 socket 均须 SO_REUSEPORT
//***********************************************
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

#include "Macro.h"
#include "noncopyable.hpp"
#include "net/Address.h"
#include "net/Socket.h"
#include "thread/Thread.h"

namespace bus {

class ThreadPool;
class Buffer;

//***********************************************
// name      : UdpServer
// brief     : UDP 服务端：接收主线程 + 任务线程池（同步收发模型）
// attention : None
//***********************************************
class UdpServer : noncopyable {
 public:
    /// @brief 数据报到达回调（**工作者线程**执行）
    /// @param data 数据报内容（shared_ptr 共享持有——回调内借 PeekAll()
    ///              视图消费，需保留可直接持有/移动该 shared_ptr；非空保证；
    ///              与 TCP 侧 MessageCallback（Buffer*）形态不同，为本模块
    ///              所有权显式传递的裁决形态）
    /// @param peer 对端地址
    using MessageCallback = std::function<void(std::shared_ptr<Buffer> data, const Address& peer)>;

    /// @brief 构造并绑定，创建接收线程对象与任务线程池（**不启动**）
    /// @param addr 绑定地址 "ip:port"（多播场景 bind "0.0.0.0:port" 后 joinGroup）
    /// @param worker_num 工作者线程数（默认 1 = 串行有序；>1 并行处理）
    /// @throw Exception::AddressError 地址格式非法
    /// @throw Exception::SocketError bind 失败（如端口占用）
    UdpServer(std::string_view addr, uint32_t worker_num = 1);

    /// @brief 析构（自动收尾：已 Start 未 Stop 则 Stop；回调内禁止析构——死锁）
    ~UdpServer();

    /// @brief 启动已创建的接收线程与任务线程池（幂等；Stop 后拒绝）
    /// @attention 数据报回调须在 Start 前设置（工作者线程启动后并发读，
    ///            Start 后 setMessageCallback 被忽略并记警告）；未 Start
    ///            直接析构安全（对象已建、线程未跑）；启动失败（线程创建
    ///            耗尽等）回收已启动部分并重建未启动对象，可重试 Start
    void Start();

    /// @brief 停止（幂等）：停收 → join 接收线程 → 停池（**排空已提交任务**：
    ///        经 ThreadPool::stopAndDrain——已入队任务由工作者线程全部执行）
    /// @attention 内核接收缓冲中的未读报文随停止丢弃（UDP 丢包语义）；
    ///            禁止在 MessageCallback 内调用（工作者 self-join 死锁：
    ///            pthread_join 得 EDEADLK 后异常逃逸致 terminate）——
    ///            停服请置原子标志由外部线程执行 Stop
    void Stop();

    /// @brief 发送数据报到指定对端（任意线程安全；数据报内核原子）
    /// @attention Stop 后拒绝（记警告）；工作者线程内可直接回包
    void sendTo(std::string_view data, const Address& peer);

    /// @brief 设置数据报回调（**须在 Start 前**——工作者线程启动后并发读，
    ///        Start 后调用被忽略并记警告）
    void setMessageCallback(MessageCallback cb) noexcept;

    /// @brief 允许发送广播（fd 级线程安全，建议 Start 前设置）
    void setBroadcast(bool on) noexcept {
        m_socket->setBroadcast(on);
    }

    /// @brief 允许端口复用（多实例同址绑定场景；须在 Start 前设置）
    /// @attention SO_REUSEPORT 与 SO_REUSEADDR 在 UDP 重复绑定下的单播
    ///            投递语义不同（前者内核按四元组哈希分发、非复制投递）
    bool setReusePort(bool on) noexcept {
        return m_socket->setReusePort(on);
    }

    /// @brief 加入 IPv4 多播组（建议 Start 前设置；须先 bind "0.0.0.0:port"）
    bool joinGroup(const Address& multicastAddr) noexcept {
        return m_socket->joinGroup(multicastAddr);
    }

    /// @brief 离开 IPv4 多播组
    bool leaveGroup(const Address& multicastAddr) noexcept {
        return m_socket->leaveGroup(multicastAddr);
    }

    /// @brief 设置多播 TTL
    void setMulticastTTL(int ttl) noexcept {
        m_socket->setMulticastTTL(ttl);
    }

    /// @brief 实际绑定地址（构造期 getsockname）
    ATTR_PURE_INLINE
    const Address& localAddr() const noexcept {
        return m_local_addr;
    }

 private:
    /// @brief 接收主线程：poll 超时轮询 + 停止检查 → 排空 recvFrom → 入池
    void recvLoop();

    /// @brief UDP socket（fd 由本对象独占，析构关闭）
    std::unique_ptr<Socket> m_socket;
    /// @brief 实际绑定地址
    Address m_local_addr;
    /// @brief 工作者线程数
    const uint32_t m_worker_num;
    /// @brief 任务线程池（构造期创建、Start 启动）
    std::unique_ptr<ThreadPool> m_pool;
    /// @brief 接收主线程（构造期创建、Start 启动）
    std::unique_ptr<Thread> m_recv_thread;
    MessageCallback m_msg_cb;
    /// @brief 是否已启动（Start 幂等标志）
    std::atomic<bool> m_started{false};
    /// @brief 是否已停止（接收循环与 sendTo 据此让路）
    std::atomic<bool> m_stopped{false};
};

}  // namespace bus
