#pragma once
#include <log/Logging.h>
#include <base/noncopyable.hpp>
#include <log/LogFormatter.h>
#include <log/LogAppender.h>
#include <thread/Mutex.hpp>
#include <base/Macro.h>
#include <list>
#include <utility>
#include <type_traits>


#define LOG_LEVEL(logger,level)  if(level >= logger.getLevel()) \
                                        Logging(logger,static_cast<SourceFile>(__FILE__),__LINE__,__FUNCTION__,level).getStream()

#define  LOG_FMT_LEVEL(logger,level,fmt,...)  if(level >= logger.getLevel()) \
                                                    Logging(logger,static_cast<SourceFile>(__FILE__),__LINE__,__FUNCTION__,level).LogFormat(fmt,##__VA_ARGS__)

#define LOG_TRACE(logger)  LOG_LEVEL(logger,LogLevel::TRACE)
#define LOG_DEBUG(logger)  LOG_LEVEL(logger,LogLevel::DEBUG)
#define LOG_INFO(logger)   LOG_LEVEL(logger,LogLevel::INFO)
#define LOG_WARN(logger)   LOG_LEVEL(logger,LogLevel::WARN)
#define LOG_ERROR(logger)  LOG_LEVEL(logger,LogLevel::ERROR)
#define LOG_FATAL(logger)  LOG_LEVEL(logger,LogLevel::FATAL)

#define LOG_FMT_TRACE(logger,fmt,...)  LOG_FMT_LEVEL(logger,LogLevel::TRACE,fmt,##__VA_ARGS__)
#define LOG_FMT_DEBUG(logger,fmt,...)  LOG_FMT_LEVEL(logger,LogLevel::DEBUG,fmt,##__VA_ARGS__)
#define LOG_FMT_INFO(logger,fmt,...)   LOG_FMT_LEVEL(logger,LogLevel::INFO,fmt,##__VA_ARGS__)
#define LOG_FMT_WARN(logger,fmt,...)   LOG_FMT_LEVEL(logger,LogLevel::WARN,fmt,##__VA_ARGS__)
#define LOG_FMT_ERROR(logger,fmt,...)  LOG_FMT_LEVEL(logger,LogLevel::ERROR,fmt,##__VA_ARGS__)
#define LOG_FMT_FATAL(logger,fmt,...)  LOG_FMT_LEVEL(logger,LogLevel::FATAL,fmt,##__VA_ARGS__)

/// @brief 抛出异常，记录日志
#define LOG_THROW(ex) \
        throw LogThrow(__FILE__,__LINE__,__FUNCTION__,ex)

/// @brief:异常断言宏，记录日志
#define ASSERT_THROW(expr,ex) \
            (LIKELY(static_cast<bool>(expr))?void(0):throw AssertFail(#expr,__FILE__,__LINE__,__FUNCTION__,std::forward<decltype(ex)>(ex)))

/// @brief 检查指针不为空，记录日志
#define CHECK_NOTNULL(val) CheckNotNull(__FILE__,__LINE__,__FUNCTION__, \
							"'" #val "' Must be non NULL", (val))

class Logger:noncopyable
{
    friend class Logging;
public:
    using LogStreamPtr=std::shared_ptr<std::stringstream>;

    explicit Logger(std::string&& name);

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

    void addAppender(const LogAppender::ptr& appender);

    void delAppender(const LogAppender::ptr& appender);

    static Logger& getRoot();
private:
    void logging(LogStreamPtr&& logStream,LogLevel level);

    std::string m_name;
	LogLevel m_level;
    /// @brief 格式化器
	LogFormatter::ptr m_format;
    std::list<LogAppender::ptr> m_appenders;
	MutexLock m_mutex;
};

#define LOG_ROOT Logger::getRoot()

template <typename T,typename =typename std::enable_if_t<std::is_pointer<T*>::value>>
constexpr T* CheckNotNull(const char* file,int line,std::string_view func,std::string_view log,T* val)
{
	if (!static_cast<bool>(val))
	{
		Logging(LOG_ROOT,static_cast<SourceFile>(file),line,func.data(),LogLevel::FATAL).getStream()<<log;
	}
	return val;
}

template<typename T>
constexpr T LogThrow(const char* file,int line,const char* func,T&& ex)
{
    Logging(LOG_ROOT,static_cast<SourceFile>(file),line,func,LogLevel::FATAL).getStream()<<"throw exception,reason:"<<ex.what();
    return ex;
}

template<typename T>
constexpr T AssertFail(const char* expr,const char* file,int line,const char* func,T&& ex)
{
    Logging(LOG_ROOT,static_cast<SourceFile>(file),line,func,LogLevel::FATAL).getStream()<<"error expression:("<<expr<<"),reason:"<<ex.what();
    return ex;
}
