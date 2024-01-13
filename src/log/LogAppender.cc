#include <log/LogAppender.h>
#include <thread/ProcessInfo.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <log/Logging.h>
#include <sys/stat.h>

namespace
{
namespace util
{
bool isDir(const char* pathname)
{
    struct stat buf{};
    if(::stat(pathname,&buf)>=0 && S_ISDIR(buf.st_mode))
    {
        return true;
    }
    else
    {
        return false;
    }

}

void CheckPathName(const char* pathname)
{
    if(!isDir(pathname))
    {
        if(::mkdir(pathname,S_IROTH|S_IXOTH |S_IRGRP|S_IWGRP|S_IXGRP|S_IRUSR|S_IWUSR|S_IXUSR|S_IFDIR)<0)
        {
            ::perror("mdkir error!");
        }
    }
}

}
}

constexpr const char* LogColor[]={
        "\033[32m",
        "\033[m"  ,
        "\033[m"  ,
        "\033[33m",
        "\033[31m",
        "\033[31m",
        "\033[0m"
};

File::File(const std::string &filename,const std::string& logPath)
:m_file(::fopen(getPathName(logPath,filename).c_str(),"ae"),[](FILE* file)->void{
    ::fclose(file);
}),
 m_buf{},
m_writtenBytes(0)
{
    assert(m_file);
    ::setbuffer(m_file.get(),m_buf,File::BUFFSIZE);
}

void File:: Write(const char *log, size_t len)
{
    size_t written=0;
    size_t remain=len;
    while(remain)
    {
        size_t n=this->WriteUnlock(log,len);
        if(n !=remain)
        {
            int errnum=::ferror_unlocked(m_file.get());
            if(errnum)
            {
                ::fprintf(stderr,"LogFile::File::append error:%s\n",::strerror(errnum));
            }
        }
        written +=n;
        remain =len-written;
    }
    m_writtenBytes+=written;
}

void File::Flush() const
{
    if(0!=::fflush_unlocked(m_file.get()))
    {
        ::perror("LogFile::File::flush error");
    }
}

size_t File::WriteUnlock(const char *log, size_t len)
{
    return ::fwrite_unlocked(log,1,len,m_file.get());
}

std::string File::getPathName(const std::string &pathname, const std::string &filename)
{
    util::CheckPathName(pathname.c_str());
    if(pathname.back()!='/')
    {
        return pathname+'/'+filename;
    }
    return pathname+filename;
}


AppendFile::AppendFile(std::string &&logName,std::string logPath, off64_t rollSize, int flushInterval, int flushLogCount)
: m_logName(std::move(logName)),m_logPath(std::move(logPath)), m_rollSize(rollSize), m_flushInterval(flushInterval), m_flushLogCount(flushLogCount),
  m_writtenCount(0), m_lastRoll(0), m_lastFlush(0)
{
    assert(m_logName.find('/')==std::string::npos);
    RollFile();
}

void AppendFile::RollFile()
{
    Timestamp now=Timestamp::now();
    if(now>m_lastRoll)
    {
        m_lastRoll=now;
        m_lastFlush=now;
        m_file.reset(new File(getRollLogFileName(m_logName,now),m_logPath));
    }
}

std::string AppendFile::getRollLogFileName(const std::string& baseName, Timestamp start)
{
    std::string fileName;
    // baseName.yyyymmdd-HHMMSS.hostname.pid.log
    fileName.assign(baseName+start.toFormatString(".%Y%m%d-%H%M%S.")
                        +ProcessInfo::hostname()+'.'+std::to_string(ProcessInfo::Pid())+".log");
    return fileName;
}

void AppendFile::AppendUnlock(const char *log, int len)
{
    m_file->Write(log,len);
    if(m_file->WrittenBytes()>m_rollSize)
    {
        RollFile();
    }
    else
    {
        ++m_writtenCount;
        //每m_flushLogSize进行检查操作，防止影响性能
        if(m_writtenCount >= m_flushLogCount)
        {
            m_writtenCount=0;
            Timestamp now=Timestamp::now();
            if(now-m_lastRoll.toDay() > static_cast<Timestamp>(Timestamp::CONS_MicroSecondPerDay))//进入下一日
            {
                RollFile();
            }
            else if(now-m_lastFlush > static_cast<Timestamp>(m_flushInterval))//固定时间刷新缓冲区，防止丢失日志
            {
                m_lastFlush=now;
                m_file->Flush();
            }
        }
    }
}

AsynAppendFile::AsynAppendFile(std::string &&logName,std::string logPath, off_t rollSize, int flushInterval, int flushLogCount)
: AppendFile(std::move(logName), std::move(logPath),rollSize, flushInterval, flushLogCount),m_isRunning(false),
m_thread([this](){
    this->threadFunc();
    },"LogAppenderThread")
{
    assert(m_isRunning == false);
    start();
}

AsynAppendFile::~AsynAppendFile()
{
    if(m_isRunning)
    {
        stop();
    }
}

void AsynAppendFile::Append(const char *log, int len)
{
    MutexLockGuard lock(m_mutex);
    AppendUnlock(log, len);
}

void AsynAppendFile::threadFunc()
{
    assert(m_isRunning==true);
    while(this->m_isRunning)
    {
        m_sem.wait();
        if(m_isRunning)
        {
            assert(!m_logBuffer.empty());
            //当前为单消费者线程模型，所以这里不加锁
            LogStreamPtr logStream = std::move(m_logBuffer.front());
            std::string log = logStream->str();
            Append(log.c_str(), log.length());
            m_logBuffer.pop();
        }
    }
    Flush();
}

void AsynAppendFile::start()
{
    m_isRunning=true;
    m_thread.run();
}

void AsynAppendFile::stop()
{
    m_isRunning=false;
    m_sem.notify();
    m_thread.join();
}

void AsynAppendFile::Append(LogStreamPtr&& logStream)
{
    m_logBuffer.emplace(std::move(logStream));
    m_sem.notify();
}

void AsynAppendFile::Flush()
{
    MutexLockGuard lock(AppendFile::m_mutex);
    FlushUnlock();
}

SyncAppendFile::SyncAppendFile(std::string &&logName, std::string logPath,off_t rollSize, int flushInterval, int flushLogCount) :
AppendFile(std::move(logName),std::move(logPath) ,rollSize, flushInterval, flushLogCount)
{

}

void SyncAppendFile::Append(const char *log, int len)
{
    MutexLockGuard lock(AppendFile::m_mutex);
    AppendUnlock(log, len);
}

void SyncAppendFile::Flush()
{
    MutexLockGuard lock(AppendFile::m_mutex);
    FlushUnlock();
}

void SyncAppendFile::Append(LogStreamPtr&& logStream)
{
    std::string log=logStream->str();
    Append(log.c_str(),static_cast<int>(log.length()));
}

FileLogAppender::FileLogAppender(std::string logName,std::string logPath, AppenderAction action, int flushInterval, int flushLogCount,
                                 off64_t singleFileSize)
:m_appendFile(action == AppenderAction::SYNC ? std::unique_ptr<AppendFile>(new SyncAppendFile(std::move(logName),std::move(logPath),singleFileSize,flushInterval,flushLogCount)):
        std::unique_ptr<AppendFile>(new AsynAppendFile(std::move(logName),std::move(logPath),singleFileSize,flushInterval,flushLogCount)))
{
}

void FileLogAppender::setSingleFileSize(off64_t singleFileSize)
{
    m_appendFile->setRollSize(singleFileSize);
}

void FileLogAppender::append(LogStreamPtr logStream)
{
    m_appendFile->Append(std::move(logStream));
}

void FileLogAppender::append(LogAppender::LogStreamPtr logStream, LogLevel level)
{
    m_appendFile->Append(std::move(logStream));
}

void StdoutLogAppender::append(LogStreamPtr logStream)
{
    std::cout<<logStream->str().c_str();
}

void StdoutLogAppender::append(LogAppender::LogStreamPtr logStream, LogLevel level)
{
    std::cout<<LogColor[static_cast<unsigned char>(level)]<<logStream->str().c_str()<<LogColor[static_cast<unsigned char>(LogLevel::NUM_LOG_LEVELS)];
}

/*
void StderrLogAppender::append(LogStreamPtr logStream)
{
    std::cerr<<logStream->str().c_str();
}

void StderrLogAppender::append(LogAppender::LogStreamPtr logStream, LogLevel level)
{
    std::cerr<<LogColor[static_cast<Byte>(level)]<<logStream->str().c_str()<<LogColor[static_cast<Byte>(LogLevel::NUM_LOG_LEVELS)];
}
*/