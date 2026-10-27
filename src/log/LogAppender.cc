#include <log/LogAppender.h>
#include <thread/ProcessInfo.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <log/Logging.h>
#include <sys/stat.h>

namespace bus {

namespace util {
bool isDir(const char* pathname) {
    struct stat buf{};
    if (::stat(pathname, &buf) >= 0 && S_ISDIR(buf.st_mode)) {
        return true;
    } else {
        return false;
    }
}

void CheckPathName(const char* pathname) {
    if (!isDir(pathname)) {
        if (::mkdir(pathname, S_IROTH | S_IXOTH | S_IRGRP | S_IWGRP | S_IXGRP | S_IRUSR | S_IWUSR | S_IXUSR | S_IFDIR) <
            0) {
            ::perror("mdkir error!");
        }
    }
}

}  // namespace util

constexpr const char* LogColor[] = {"\033[m", "\033[m", "\033[32m", "\033[33m", "\033[31m", "\033[31m", "\033[0m"};

File::File(const std::string& filename, const std::string& logPath)
    : m_file(::fopen(getPathName(logPath, filename).c_str(), "ae"), [](FILE* file) -> void { ::fclose(file); }),
      m_buf{},
      m_writtenBytes(0) {
    assert(m_file);
    ::setbuffer(m_file.get(), m_buf, File::BUFFSIZE);
}

void File::Write(const char* log, size_t len) {
    size_t written = 0;
    size_t remain = len;
    while (remain) {
        size_t n = this->WriteUnlock(log, len);
        if (n != remain) {
            int errnum = ::ferror_unlocked(m_file.get());
            if (errnum) {
                ::fprintf(stderr, "LogFile::File::append error:%s\n", ::strerror(errnum));
            }
        }
        written += n;
        remain = len - written;
    }
    m_writtenBytes += written;
}

void File::Flush() const {
    if (0 != ::fflush_unlocked(m_file.get())) {
        ::perror("LogFile::File::flush error");
    }
}

size_t File::WriteUnlock(const char* log, size_t len) {
    return ::fwrite_unlocked(log, 1, len, m_file.get());
}

std::string File::getPathName(const std::string& pathname, const std::string& filename) {
    util::CheckPathName(pathname.c_str());
    if (pathname.back() != '/') {
        return pathname + '/' + filename;
    }
    return pathname + filename;
}

AppendFile::AppendFile(std::string&& logName, std::string logPath, off64_t rollSize, int flushInterval,
                       int flushLogCount)
    : m_logName(std::move(logName)),
      m_logPath(std::move(logPath)),
      m_rollSize(rollSize),
      m_flushInterval(flushInterval),
      m_flushLogCount(flushLogCount),
      m_writtenCount(0),
      m_lastRoll(0),
      m_lastFlush(0) {
    assert(m_logName.find('/') == std::string::npos);
    RollFile();
}

void AppendFile::RollFile() {
    Timestamp now = Timestamp::now();
    m_lastRoll = now;
    m_lastFlush = now;
    ++m_rollCount;
    m_file = std::make_unique<File>(getRollLogFileName(m_logName, now), m_logPath);
}

std::string AppendFile::getRollLogFileName(const std::string& baseName, Timestamp start) {
    std::string fileName;
    // baseName.yyyymmdd-HHMMSS-<序号>.hostname.pid.log（序号保证同秒滚动文件名唯一）
    fileName.assign(baseName + start.toFormatString(".%Y%m%d-%H%M%S-") + std::to_string(m_rollCount) + '.' +
                    ProcessInfo::hostname() + '.' + std::to_string(ProcessInfo::Pid()) + ".log");
    return fileName;
}

void AppendFile::AppendUnlock(const char* log, int len) {
    m_file->Write(log, len);
    if (m_file->WrittenBytes() > m_rollSize) {
        RollFile();
    } else {
        ++m_writtenCount;
        // 每m_flushLogSize进行检查操作，防止影响性能
        if (m_writtenCount >= m_flushLogCount) {
            m_writtenCount = 0;
            Timestamp now = Timestamp::now();
            if (now - m_lastRoll.toDay() > static_cast<Timestamp>(Timestamp::CONS_MicroSecondPerDay))  // 进入下一日
            {
                RollFile();
            } else if (now - m_lastFlush > static_cast<Timestamp>(m_flushInterval))  // 固定时间刷新缓冲区，防止丢失日志
            {
                m_lastFlush = now;
                m_file->Flush();
            }
        }
    }
}

AsynAppendFile::AsynAppendFile(std::string&& logName, std::string logPath, off_t rollSize, int flushInterval,
                               int flushLogCount)
    : AppendFile(std::move(logName), std::move(logPath), rollSize, flushInterval, flushLogCount),
      m_isRunning(false),
      m_logBuffer{},
      m_sem{0},
      m_mutex{},
      m_thread([this]() { this->threadFunc(); }, "LogAppenderThread") {
    assert(m_isRunning == false);
    start();
}

AsynAppendFile::~AsynAppendFile() {
    if (m_isRunning) {
        stop();
    }
}

void AsynAppendFile::Append(const char* log, int len) {
    // MutexLockGuard lock(m_mutex);
    AppendUnlock(log, len);
}

void AsynAppendFile::threadFunc() {
    assert(m_isRunning == true);
    while (this->m_isRunning) {
        m_sem.wait();
        if (m_isRunning) {
            // 与生产者（Append）共用 m_mutex 保护队列（D32）：持锁取出、
            // 锁外落盘（避免文件 IO 阻塞生产者入队）
            std::string log;
            {
                MutexLockGuard lock(m_mutex);
                if (m_logBuffer.empty()) {
                    continue;  // sem 唤醒与队列状态在锁外存在竞态，空则重等
                }
                log = std::move(m_logBuffer.front());
                m_logBuffer.pop();
            }
            AppendUnlock(log.c_str(), static_cast<int>(log.length()));
        }
    }
    // D31：退出前排空队列（stop 语义为 drain，已入队日志全部落盘）
    while (true) {
        std::string log;
        {
            MutexLockGuard lock(m_mutex);
            if (m_logBuffer.empty()) {
                break;
            }
            log = std::move(m_logBuffer.front());
            m_logBuffer.pop();
        }
        AppendUnlock(log.c_str(), static_cast<int>(log.length()));
    }
    Flush();
}

void AsynAppendFile::start() {
    MutexLockGuard lock(m_mutex);
    if (!m_isRunning) {
        m_isRunning = true;
        m_thread.run();
    }
}

void AsynAppendFile::stop() {
    {
        MutexLockGuard lock(m_mutex);
        if (!m_isRunning) {
            return;
        }
        m_isRunning = false;
    }
    m_sem.notify();
    m_thread.join();  // 锁外 join：等待期间不阻塞生产者
}

void AsynAppendFile::Append(std::string_view logStream) {
    {
        MutexLockGuard lock(m_mutex);
        m_logBuffer.emplace(logStream.data(), logStream.size());
    }
    m_sem.notify();
}

void AsynAppendFile::Flush() {
    MutexLockGuard lock(AppendFile::m_mutex);
    FlushUnlock();
}

SyncAppendFile::SyncAppendFile(std::string&& logName, std::string logPath, off_t rollSize, int flushInterval,
                               int flushLogCount)
    : AppendFile(std::move(logName), std::move(logPath), rollSize, flushInterval, flushLogCount) {}

void SyncAppendFile::Append(const char* log, int len) {
    MutexLockGuard lock(AppendFile::m_mutex);
    AppendUnlock(log, len);
}

void SyncAppendFile::Flush() {
    MutexLockGuard lock(AppendFile::m_mutex);
    FlushUnlock();
}

void SyncAppendFile::Append(std::string_view logStream) {
    Append(logStream.data(), static_cast<int>(logStream.length()));
}

FileLogAppender::FileLogAppender(std::string logName, std::string logPath, AppenderAction action, int flushInterval,
                                 int flushLogCount, off64_t singleFileSize)
    : m_appendFile(action == AppenderAction::SYNC
                       ? std::unique_ptr<AppendFile>(new SyncAppendFile(std::move(logName), std::move(logPath),
                                                                        singleFileSize, flushInterval, flushLogCount))
                       : std::unique_ptr<AppendFile>(new AsynAppendFile(
                             std::move(logName), std::move(logPath), singleFileSize, flushInterval, flushLogCount))) {}

void FileLogAppender::setSingleFileSize(off64_t singleFileSize) {
    m_appendFile->setRollSize(singleFileSize);
}

void FileLogAppender::append(std::string_view logStream) {
    m_appendFile->Append(logStream);
}

void FileLogAppender::append(std::string_view logStream, LogLevel level) {
    m_appendFile->Append(logStream);
}

void StdoutLogAppender::append(std::string_view logStream) {
    std::cout << logStream;
}

void StdoutLogAppender::append(std::string_view logStream, LogLevel level) {
    std::cout << LogColor[static_cast<unsigned char>(level)] << logStream
              << LogColor[static_cast<unsigned char>(LogLevel::NUM_LOG_LEVELS)];
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

}  // namespace bus