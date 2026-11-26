//***********************************************
// name      : HttpConnectionPool.cc
// brief     : 单 host 连接池实现（ADR-0003：N 实例调度层）
// attention : 池状态（entries/queue/busy）loop 线程独占；跨线程入口经 QueueInLoop
//***********************************************
#include "net/http/HttpConnectionPool.h"

#include <algorithm>

#include "log/Logger.h"
#include "net/EventLoop.h"

namespace bus::http {

static Logger* g_logger = GET_LOGGER("sys");

HttpConnectionPool::HttpConnectionPool(EventLoop* loop, std::string_view addr, HttpPoolConfig cfg)
    : m_loop(loop), m_addr(addr), m_cfg(cfg) {
}

HttpConnectionPool::~HttpConnectionPool() noexcept {
    // 契约：须 Stop 完成（done cb 触发、连接全断）后析构——实例 TcpClient 析构断言兜底
}

void HttpConnectionPool::Start() {
    m_stopped.store(false);
}

void HttpConnectionPool::Stop(DoneCallback cb) {
    if (m_stopped.exchange(true)) {
        // 重复 Stop：done cb 仍须兑现（链入收口；测试 body+TearDown 双 Stop 形态）
        if (cb) {
            m_loop->QueueInLoop([this, cb = std::move(cb)]() mutable {
                if (m_stop_closed && m_stop_barrier && m_stop_pending == 0) {
                    cb();  // 前次收口已完成
                } else {
                    auto prev = std::move(m_stop_cb);
                    m_stop_cb = [prev = std::move(prev), cb = std::move(cb)]() mutable {
                        if (prev) {
                            prev();
                        }
                        cb();
                    };
                }
            });
        }
        return;
    }
    m_loop->QueueInLoop([this, cb = std::move(cb)]() mutable {
        // 排队请求 nullopt 交付（exactly-once：队列取走即清）
        for (auto& q : m_queue) {
            if (q.second) {
                q.second(std::nullopt);
            }
        }
        m_queue.clear();
        m_stop_cb = std::move(cb);
        m_stop_pending = 0;
        m_stop_closed = true;
        for (auto& e : m_entries) {
            if (e->client->connection()) {
                ++m_stop_pending;
                // 观察者经 RunInLoop 装配，FIFO 先于 Stop 清理闭包（D64 同款）
                e->client->setDisconnectedObserver([this] {
                    if (m_stop_pending > 0) {
                        --m_stop_pending;
                    }
                    maybeFireStopDone();
                });
            }
            e->client->Stop();  // 在途回调 nullopt 经实例收口（exactly-once）
        }
        // FIFO 屏障：本闭包在所有实例 Stop 清理闭包之后执行（排队序保证）——
        // parked/无连接实例的 cb 收口先于 done，done 后析构安全（审查修复）
        m_loop->QueueInLoop([this] {
            m_stop_barrier = true;
            maybeFireStopDone();
        });
        maybeFireStopDone();
    });
}

void HttpConnectionPool::maybeFireStopDone() {
    if (m_stop_barrier && m_stop_pending == 0 && m_stop_cb) {
        auto done = std::move(m_stop_cb);
        m_stop_cb = nullptr;
        done();
    }
}

void HttpConnectionPool::Get(std::string_view path_and_query, ResponseCallback cb) {
    HttpRequest req;
    req.setMethod(HttpMethod::kGET);
    req.appendUrl(path_and_query);
    req.setPath(std::string(path_and_query));
    Request(std::move(req), std::move(cb));
}

void HttpConnectionPool::Post(std::string_view path, std::string_view content_type, std::string_view body,
                              ResponseCallback cb) {
    HttpRequest req;
    req.setMethod(HttpMethod::kPOST);
    req.appendUrl(path);
    req.setPath(std::string(path));
    req.addHeaderRaw("Content-Type", content_type);
    req.appendBody(body);
    Request(std::move(req), std::move(cb));
}

void HttpConnectionPool::Request(HttpRequest req, ResponseCallback cb) {
    enqueue(std::move(req), std::move(cb));
}

void HttpConnectionPool::enqueue(HttpRequest req, ResponseCallback cb) {
    m_loop->QueueInLoop([this, req = std::move(req), cb = std::move(cb)]() mutable {
        if (m_stopped.load()) {
            if (cb) {
                cb(std::nullopt);
            }
            return;
        }
        m_queue.emplace_back(std::move(req), std::move(cb));
        dispatch();
    });
}

void HttpConnectionPool::dispatch() {
    const uint32_t cap = std::max(1u, m_cfg.maxConnections);
    while (!m_queue.empty() && !m_stopped.load()) {
        Entry* idle = nullptr;
        for (auto& e : m_entries) {
            if (!e->busy) {
                idle = e.get();
                break;
            }
        }
        if (idle == nullptr) {
            if (m_entries.size() >= cap) {
                break;  // 全忙且达上限：留队等响应完成分发
            }
            // 惰性创建（Entry 经 unique_ptr 持有，vector 扩容不移动 Entry 本体——裸指针稳定）
            auto e = std::make_unique<Entry>();
            e->client = std::make_unique<HttpClient>(m_loop, m_addr, m_cfg.client);
            e->client->Start();
            idle = e.get();
            m_entries.push_back(std::move(e));
        }
        auto item = std::move(m_queue.front());
        m_queue.pop_front();
        idle->busy = true;
        Entry* ep = idle;
        idle->client->Request(std::move(item.first),
                              [this, ep, cb = std::move(item.second)](std::optional<HttpResponse> r) mutable {
                                  ep->busy = false;
                                  if (cb) {
                                      cb(std::move(r));  // 用户回调先交付（可重入 Request/Stop）
                                  }
                                  dispatch();  // 响应完成即分发排队请求
                              });
    }
}

}  // namespace bus::http