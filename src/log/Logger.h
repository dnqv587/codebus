#pragma once
#include <log/Logging.h>
#include <base/noncopyable.hpp>
#include <log/LogFormatter.h>
#include <log/LogAppender.h>
#include <thread/Mutex.hpp>
#include <list>
#include <utility>
#include <type_traits>


#define LOG_LEVEL(logger,level)  if(level >= logger.getLevel()) \
                                        Loggin(logger,__FILE__,__LINE__,__FUNCTION__,level).getStream()

#define LOG_TRACE(logger)  LOG_LEVEL(logger,LogLevel::TRACE)
#define LOG_DEBUG(logger)  LOG_LEVEL(logger,LogLevel::DEBUG)
#define LOG_INFO(logger)   LOG_LEVEL(logger,LogLevel::INFO)
#define LOG_WARN(logger)   LOG_LEVEL(logger,LogLevel::WARN)
#define LOG_ERROR(logger)  LOG_LEVEL(logger,LogLevel::ERROR)
#define LOG_FATAL(logger)  LOG_LEVEL(logger,LogLevel::FATAL)

#define LOG_ROOT Logger::getRoot();


class Logger:noncopyable
{
    friend class Loggin;
public:
    using LogStreamPtr=std::shared_ptr<std::stringstream>;

    explicit Logger(std::string&& name);

	void trace(const char* fmt,...)
	{

	}

    std::string getName() const
	{return m_name;}

    LogLevel getLevel() const
	{return m_level;}

	void setLevel(LogLevel level)
	{m_level=level;}

	void setFormatter(LogFormatter::ptr formatter)
    {m_format=std::move(formatter);}
	void setFormatter(const std::string &pattern)
    {m_format = std::make_shared<LogFormatter>(pattern);}

    LogFormatter::ptr getFormatter() const {return m_format;}

    void addAppender(LogAppender::ptr appender);

    void delAppender(LogAppender::ptr appender);

    static Logger& getRoot();
private:
    void loggin(LogStreamPtr&& logStream,LogLevel level);

    std::string m_name;
	LogLevel m_level;
    /// @brief 格式化器
	LogFormatter::ptr m_format;
    std::list<LogAppender::ptr> m_appenders;
	MutexLock m_mutex;
};
