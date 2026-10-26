#pragma once
#include <noncopyable.hpp>
#include <BlockingQueue.hpp>
#include <thread/Thread.h>
#include <functional>
#include <vector>
#include <atomic>

namespace bus
{
//***********************************************
// class     : ThreadPool
// brief     : 线程池
// attention : None
// date      : 2023/10/19
//***********************************************
class ThreadPool : noncopyable
{
 public:
	using ptr = std::shared_ptr<ThreadPool>;
	using Task = std::function<void()>;

	explicit ThreadPool(std::string name);
	~ThreadPool();
	/// @brief 启动线程池
	/// @param threadNum 线程数
	void start(uint32_t threadNum = CORE_NUM);
	/// @brief 停止线程池（丢弃已入队未执行任务）
	void stop();
	/// @brief 停止线程池并排空：已入队任务由**工作者线程**全部执行完毕后
	///        才返回（排空段在原处理循环退出后于工作者线程内进行）
	/// @attention 与 stop() 的区别：本函数保证已入队任务执行；入队侧
	///            stop 后 run() 仍丢弃；与 stop() 互斥且均不可重复调用
	///            （Thread::join 不幂等）
	void stopAndDrain();
	/// @brief 分配运行任务给线程吃
	/// @param task CallBack,void (*)()
	void run(Task task);

 private:
	/// @brief 核心数
	static const uint32_t CORE_NUM;
	void threadFunc();

	/// @brief 任务队列
	BlockingQueue<Task> m_taskQueue;
	/// @brief 线程容器
	std::vector<Thread::ptr> m_threads;
	/// @brief 线程池是否运行
	std::atomic<bool> m_isRunning;
	/// @brief 停止时是否排空（仅 stopAndDrain 置位；stop 保持丢弃语义）
	std::atomic<bool> m_drain_on_stop{false};
	/// @brief 线程池名
	const std::string m_name;
};
}