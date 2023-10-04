#pragma once
#include <base/noncopyable.hpp>
#include <thread/Mutex.hpp>
#include <time/Timestamp.h>
#include <sstream>
#include <stdio.h>
#include <memory>
#include <functional>

class File:noncopyable
{
public:
    static const int BUFFSIZE=64*1024;  //64K

    File(const std::string& filename);

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

class LogFile:noncopyable
{
public:
    virtual ~LogFile(){}
    virtual void Append(const char* log,int len)=0;

    void RollFile();
private:
    void AppendUnlock(const char* log,int len);
    std::string getRollLogFileName();

    File m_file;
    MutexLock m_mutex;

    /// @brief 日志名
    const std::string m_logName;
    /// @brief 单日志文件限制大小
    const off_t m_rollSize;
    /// @brief 刷新间隔
    const int m_flushInerval;
    /// @brief 缓冲区大小
    const int m_bufferSize;
    /// @brief 已写入缓冲区size
    const int m_writtenSize;

    /// @brief 日志开始时间[s]
    Timestamp m_startPeriod;
    /// @brief 最新文件刷新时间
    Timestamp m_lastRoll;
    /// @brief 最新缓冲区刷新时间
    Timestamp m_lastFlush;
};

class AsynLogFile:public LogFile,
                    noncopyable
{
public:
    AsynLogFile()

private:

};


class LogAppender
{
public:
    ~LogAppender(){}

    virtual void append(std::stringstream&& stream)=0;
};

class FileLogAppender:LogAppender,
                        noncopyable
{
public:

private:

};

