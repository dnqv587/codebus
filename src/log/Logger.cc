#include <log/Logger.h>
#include <memory>
#include <algorithm>

Logger::Logger(std::string &&name)
:m_name(name),m_level(LogLevel::DEBUG),
m_format(nullptr)
{

}

Logger &Logger::getRoot()
{
    static Logger logger("Root");
    return logger;
}

void Logger::addAppender(const LogAppender::ptr& appender)
{
    MutexLockGuard lock(m_mutex);
    m_appenders.push_back(appender);
}

void Logger::delAppender(const LogAppender::ptr& appender)
{
    MutexLockGuard lock(m_mutex);
   auto iter = std::find(m_appenders.begin(), m_appenders.end(),appender);
   if(iter != m_appenders.cend())
   {
       m_appenders.erase(iter);
   }
}

void Logger::logging(LogStreamPtr&& logStream,LogLevel level)
{
    MutexLockGuard lock(m_mutex);
    for(auto& appender:m_appenders)
    {
        appender->append(logStream,level);
    }
}