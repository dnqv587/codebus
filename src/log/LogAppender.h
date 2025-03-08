#pragma once
#include <sstream>
#include <cstdio>
#include <memory>
#include <functional>
#include <atomic>
#include <queue>
#include "base/noncopyable.hpp"
#include "thread/Mutex.hpp"
#include "time/Timestamp.h"
#include"thread/Thread.h"
#include "thread/Semaphore.hpp"

namespace bus
{
class File : noncopyable
{
 public:
	static const int BUFFSIZE = 64 * 1024;  //64K

	File(const std::string& filename, const std::string& logPath);

	void Write(const char* log, size_t len);

	void Flush() const;
	ATTR_PURE_INLINE
	off64_t WrittenBytes() const
	{
		return m_writtenBytes;
	}

 private:
	size_t WriteUnlock(const char* log, size_t len);

	static std::string getPathName(const std::string& pathname, const std::string& filename);

	std::unique_ptr<FILE, std::function<void(FILE*)>> m_file;
	/// @brief 用户态缓冲区
	char m_buf[BUFFSIZE];
	/// @brief 已写入大小
	off64_t m_writtenBytes;
};

class AppendFile
{
 public:
	/// @brief
	/// @param logName 日志名
	/// @param rollSize 单文件限制大小
	/// @param flushInterval 缓冲区刷新时间
	/// @param flushLogCount 缓冲区刷新间隔计数
	AppendFile(std::string&& logName, std::string logPath, off64_t rollSize, int flushInterval, int flushLogCount);
	virtual ~AppendFile() = default;
	virtual void Append(std::string_view logStream) = 0;

	virtual void Flush() = 0;

	void RollFile();

	void setRollSize(off_t rollSize)
	{
		m_rollSize = rollSize;
	}

 protected:
	void AppendUnlock(const char* log, int len);
	void FlushUnlock()
	{
		m_file->Flush();
	}
	std::string getRollLogFileName(const std::string& baseName, Timestamp start);

	std::unique_ptr<File> m_file;
	MutexLock m_mutex;

	/// @brief 日志名
	const std::string m_logName;
	/// @brief 日志路径
	const std::string m_logPath;
	/// @brief 单日志文件限制大小
	off64_t m_rollSize;
	/// @brief 刷新间隔
	const int m_flushInterval;
	/// @brief 缓冲区刷新间隔大小
	const int m_flushLogCount;
	/// @brief 已写入缓冲区size
	size_t m_writtenCount;

	/// @brief 最新文件刷新时间
	Timestamp m_lastRoll;
	/// @brief 最新缓冲区刷新时间
	Timestamp m_lastFlush;
};

class SyncAppendFile : public AppendFile, noncopyable
{
 public:

	/// @brief 同步日志落地
	/// @param logName 日志名
	/// @param rollSize 单文件限制大小
	/// @param flushInterval 缓冲区刷新时间
	/// @param flushLogCount 缓冲区刷新间隔计数
	SyncAppendFile(std::string&& logName,
		std::string logPath,
		off_t rollSize,
		int flushInterval = 3,
		int flushLogCount = 1024);
	~SyncAppendFile() override = default;

	void Append(std::string_view logStream) override;

	void Flush() override;

 private:
	void Append(const char* log, int len);

};

class AsynAppendFile : public AppendFile, noncopyable
{
 public:
	/// @brief 异步日志落地
	/// @param logName 日志名
	/// @param rollSize 单文件限制大小
	/// @param flushInterval 缓冲区刷新时间
	/// @param flushLogCount 缓冲区刷新间隔计数
	AsynAppendFile(std::string&& logName,
		std::string logPath,
		off_t rollSize,
		int flushInterval = 3,
		int flushLogCount = 1024);

	~AsynAppendFile() override;

	void Append(std::string_view logStream) override;

	void start();

	void stop();

 private:
	void Append(const char* log, int len);
	void Flush() override;

	void threadFunc();

	//std::queue<std::reference_wrapper<const std::stringstream>> m_logBuffer;
	std::queue<std::string> m_logBuffer;
	std::atomic<bool> m_isRunning;

	MutexLock m_mutex;
	Semaphore m_sem;
	Thread m_thread;

};

enum class LogLevel : unsigned char;
class LogAppender
{
 public:
	using ptr = std::shared_ptr<LogAppender>;
	using unique_ptr = std::unique_ptr<LogAppender>;
	using ref = std::reference_wrapper<LogAppender>;

	LogAppender() = default;
	virtual ~LogAppender() = default;

	virtual void append(std::string_view logStream) = 0;
	virtual void append(std::string_view logStream, LogLevel level) = 0;
};

enum class AppenderAction
{
	/// @brief 同步
	SYNC,
	/// @brief 异步
	ASYNC
};

class FileLogAppender : public LogAppender, noncopyable
{
 public:
	explicit FileLogAppender(std::string logName,
		std::string logPath,
		AppenderAction action = AppenderAction::SYNC,
		int flushInterval = 3,
		int flushLogCount = 1024,
		off64_t singleFileSize = 4_MB);

	void append(std::string_view logStream) override;
	void append(std::string_view logStream, LogLevel level) override;

	void setSingleFileSize(off64_t singleFileSize);

 private:
	std::unique_ptr<AppendFile> m_appendFile;
};

class StdoutLogAppender : public LogAppender, noncopyable
{
 public:
	StdoutLogAppender() = default;
	~StdoutLogAppender() override = default;

	void append(std::string_view logStream) override;
	void append(std::string_view logStream, LogLevel level) override;
};

/*
class StderrLogAppender: public LogAppender, noncopyable
{
public:
    StderrLogAppender()=default;
    ~StderrLogAppender() override=default;

    void append(LogStreamPtr logStream) override;
    void append(LogStreamPtr logStream,LogLevel level) override;
};
*/

}