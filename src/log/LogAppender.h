#pragma once
#include <base/noncopyable.hpp>
#include <thread/Mutex.hpp>
#include <time/Timestamp.h>
#include<thread/Thread.h>
#include <thread/Semaphore.hpp>
#include <sstream>
#include <cstdio>
#include <memory>
#include <functional>
#include <atomic>
#include <queue>

class File:noncopyable
{
public:
    static const int BUFFSIZE=64*1024;  //64K

    explicit File(const std::string& filename);

    void Write(const char* log,size_t len);

    void Flush() const;

    off_t WrittenBytes() const
    {return m_writtenBytes;}

private:
    size_t WriteUnlock(const char* log,size_t len);

    std::unique_ptr<FILE,std::function<void (FILE*)>>  m_file;
    /// @brief 用户态缓冲区
    char m_buf[BUFFSIZE];
    /// @brief 已写入大小
    off_t m_writtenBytes;
};

class AppendFile
{
public:
    using LogStreamPtr=std::shared_ptr<std::stringstream>;
    /// @brief
    /// @param logName 日志名
    /// @param rollSize 单文件限制大小
    /// @param flushInterval 缓冲区刷新时间
    /// @param flushLogCount 缓冲区刷新间隔计数
    AppendFile(std::string&& logName,off64_t rollSize,int flushInterval,int flushLogCount);
    virtual ~AppendFile()=default;
    virtual void Append(LogStreamPtr&& logStream)=0;

    virtual void Flush()=0;

    void RollFile();

    void setRollSize(off_t rollSize)
    {m_rollSize=rollSize;}

protected:
    void AppendUnlock(const char* log,int len);
    void FlushUnlock()
    {m_file->Flush();}
    std::string getRollLogFileName(const std::string& baseName, Timestamp start);

    std::unique_ptr<File> m_file;
    MutexLock m_mutex;

    /// @brief 日志名
    const std::string m_logName;
    /// @brief 单日志文件限制大小
    off64_t m_rollSize;
    /// @brief 刷新间隔
    const int m_flushInerval;
    /// @brief 缓冲区刷新间隔大小
    const int m_flushLogCount;
    /// @brief 已写入缓冲区size
    size_t m_writtenCount;

    /// @brief 最新文件刷新时间
    Timestamp m_lastRoll;
    /// @brief 最新缓冲区刷新时间
    Timestamp m_lastFlush;
};


class SyncAppendFile:public AppendFile,noncopyable
{
public:

    /// @brief 同步日志落地
    /// @param logName 日志名
    /// @param rollSize 单文件限制大小
    /// @param flushInterval 缓冲区刷新时间
    /// @param flushLogCount 缓冲区刷新间隔计数
    SyncAppendFile(std::string&& logName,off_t rollSize,int flushInterval=3,int flushLogCount=1024);
    ~SyncAppendFile() override = default;

    void Append(LogStreamPtr&& logStream) override;

    void Flush() override;


private:
    void Append(const char* log,int len);

};

class AsynAppendFile:public AppendFile,noncopyable
{
public:
    using LogStreamPtr=std::shared_ptr<std::stringstream>;
    /// @brief 异步日志落地
    /// @param logName 日志名
    /// @param rollSize 单文件限制大小
    /// @param flushInterval 缓冲区刷新时间
    /// @param flushLogCount 缓冲区刷新间隔计数
    AsynAppendFile(std::string&& logName,off_t rollSize,int flushInterval=3,int flushLogCount=1024);

    ~AsynAppendFile() override;

    void Append(LogStreamPtr&& logStream) override;

    void start();

    void stop();

private:
    void Append(const char* log,int len);
    void Flush() override;

    void threadFunc();

    //std::queue<std::reference_wrapper<const std::stringstream>> m_logBuffer;
    std::queue<LogStreamPtr> m_logBuffer;
    std::atomic<bool> m_isRunning;

    Thread m_thread;
    MutexLock m_mutex;
    Semaphore m_sem;

};

enum class LogLevel:unsigned char;
class LogAppender
{
public:
    using ptr=std::shared_ptr<LogAppender>;
    using unique_ptr=std::unique_ptr<LogAppender>;
    using LogStreamPtr=std::shared_ptr<std::stringstream>;

    LogAppender()=default;
    virtual ~LogAppender()=default;

    virtual void append(LogStreamPtr logStream)=0;
    virtual void append(LogStreamPtr logStream,LogLevel level)=0;
};

enum class  AppenderAction
{
    /// @brief 同步
    SYNC ,
    /// @brief 异步
    ASYNC
};


class FileLogAppender: public LogAppender,noncopyable
{
public:
    explicit FileLogAppender(std::string&& logName,AppenderAction action=AppenderAction::SYNC,int flushInterval=3,int flushLogCount=1024,off64_t singleFileSize=4_MB);

    void append(LogStreamPtr logStream) override;
    void append(LogStreamPtr logStream,LogLevel level) override;

    void setSingleFileSize(off64_t singleFileSize);

private:
    std::unique_ptr<AppendFile> m_appendFile;
};

class StdoutLogAppender: public LogAppender, noncopyable
{
public:
    StdoutLogAppender()=default;
    ~StdoutLogAppender() override=default;

    void append(LogStreamPtr logStream) override;
    void append(LogStreamPtr logStream,LogLevel level) override;
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
