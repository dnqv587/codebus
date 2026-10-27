#include <thread/ThreadPool.h>
#include <unistd.h>
#include <cassert>
#include "Util.h"

namespace bus {
const uint32_t ThreadPool::CORE_NUM = SystemUtil::getCoreNum();

ThreadPool::ThreadPool(std::string name) : m_isRunning(false), m_name(std::move(name)) {}

ThreadPool::~ThreadPool() {
    if (m_isRunning) {
        stop();
    }
}

void ThreadPool::start(const uint32_t threadNum) {
    assert(m_threads.empty() && !m_isRunning);
    m_isRunning = true;
    m_threads.reserve(threadNum);
    for (int i = 0; i < threadNum; ++i) {
        m_threads.emplace_back(
            new Thread([this]() -> void { this->threadFunc(); }, m_name + "[" + std::to_string(i) + "]"));
        m_threads[i]->run();
    }
}

void ThreadPool::threadFunc() {
    while (LIKELY(m_isRunning)) {
        Task task = std::move(m_taskQueue.take());
        if (LIKELY(task)) {
            task();
        }
    }
    // stopAndDrain 请求时的排空段（D41，用户裁决形态：原处理循环不动，
    // 停止后在**工作者线程内**继续处理队列残余；stop() 普通停止不进入
    // ——丢弃语义保留）。take() 的等待谓词为「运行中且空」——over() 后
    // 空队列立即返回空任务（不阻塞），排空以取尽+空任务收尾
    if (m_drain_on_stop.load()) {
        for (;;) {
            Task task = std::move(m_taskQueue.take());
            if (!task) {
                break;
            }
            task();
        }
    }
    m_drain_on_stop.store(false);
}

void ThreadPool::stopAndDrain() {
    m_drain_on_stop.store(true);
    m_isRunning = false;
    // 空等中的工作者被 over() 唤醒（取空退出原循环 → 进排空段取空收尾）；
    // 调用方仅 join 等待——残余任务全部在工作者线程执行，不搬运到调用方
    m_taskQueue.over();
    for (const Thread::ptr& thread : m_threads) {
        thread->join();
    }
}

void ThreadPool::stop() {
    m_isRunning = false;
    m_taskQueue.over();
    for (const Thread::ptr& thread : m_threads) {
        thread->join();
    }
}

void ThreadPool::run(ThreadPool::Task task) {
    if (!m_isRunning) {
        return;
    }
    if (m_threads.empty()) {
        task();
    } else if (m_isRunning) {
        m_taskQueue.put(std::move(task));
    }
}
}  // namespace bus
