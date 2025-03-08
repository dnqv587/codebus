#pragma once
#include <utility>
#include <type_traits>
#include "base/noncopyable.hpp"
#include "log/Logging.h"
#include "log/LogFormatter.h"
#include "log/LogAppender.h"
#include "thread/Mutex.hpp"
#include "base/Macro.h"
#include "util/Util.h"
#include "base/Singleton.hpp"

namespace bus
{
#define LOG_LEVEL(logger, level)  if(level >= logger.getLevel()) \
                                        Logging(logger,StringUtil::BaseFileName(__FILE__),__LINE__,__FUNCTION__,level).getStream()

#define  LOG_FMT_LEVEL(logger, level, fmt, ...)  if(level >= logger.getLevel()) \
                                                    Logging(logger,StringUtil::BaseFileName(__FILE__),__LINE__,__FUNCTION__,level).LogFormat(fmt,##__VA_ARGS__)

#define LOG_TRACE(logger)  LOG_LEVEL(logger,LogLevel::TRACE)
#define LOG_DEBUG(logger)  LOG_LEVEL(logger,LogLevel::DEBUG)
#define LOG_INFO(logger)   LOG_LEVEL(logger,LogLevel::INFO)
#define LOG_WARN(logger)   LOG_LEVEL(logger,LogLevel::WARN)
#define LOG_ERROR(logger)  LOG_LEVEL(logger,LogLevel::ERROR)
#define LOG_FATAL(logger)  LOG_LEVEL(logger,LogLevel::FATAL)

#define LOG_FMT_TRACE(logger, fmt, ...)  LOG_FMT_LEVEL(logger,LogLevel::TRACE,fmt,##__VA_ARGS__)
#define LOG_FMT_DEBUG(logger, fmt, ...)  LOG_FMT_LEVEL(logger,LogLevel::DEBUG,fmt,##__VA_ARGS__)
#define LOG_FMT_INFO(logger, fmt, ...)   LOG_FMT_LEVEL(logger,LogLevel::INFO,fmt,##__VA_ARGS__)
#define LOG_FMT_WARN(logger, fmt, ...)   LOG_FMT_LEVEL(logger,LogLevel::WARN,fmt,##__VA_ARGS__)
#define LOG_FMT_ERROR(logger, fmt, ...)  LOG_FMT_LEVEL(logger,LogLevel::ERROR,fmt,##__VA_ARGS__)
#define LOG_FMT_FATAL(logger, fmt, ...)  LOG_FMT_LEVEL(logger,LogLevel::FATAL,fmt,##__VA_ARGS__)

/// @brief 获取Root日志节点
#define LOG_ROOT LoggerManager::getInstance()->getRoot()
#define GET_LOGGER(x) LoggerManager::getInstance()->getLogger(x)

/// @brief 抛出异常，记录日志
#define LOG_THROW(ex) \
        throw LogThrow(StringUtil::BaseFileName(__FILE__),__LINE__,__FUNCTION__,ex)

/// @brief:异常断言宏，记录日志
#define ASSERT_THROW(expr, ex) \
            (LIKELY(static_cast<bool>(expr))?void(0):throw AssertFail(#expr,StringUtil::BaseFileName(__FILE__),__LINE__,__FUNCTION__,std::forward<decltype(ex)>(ex)))

/// @brief 检查指针不为空，记录日志
#define CHECK_NOTNULL(val) CheckNotNull(StringUtil::BaseFileName(__FILE__),__LINE__,__FUNCTION__, \
                            "'" #val "' Must be non NULL", (val))

class Logger : noncopyable
{
	friend class Logging;
 public:
	using ref = std::reference_wrapper<Logger>;
	using ptr = std::shared_ptr<Logger>;
	using unique_ptr = std::unique_ptr<Logger>;

	explicit Logger(std::string&& name, LogLevel level = LogLevel::DEBUG);
	explicit Logger(const std::string& name, LogLevel level = LogLevel::DEBUG);
	ATTR_PURE_INLINE
	std::string getName() const
	{
		return m_name;
	}
	ATTR_PURE_INLINE
	LogLevel getLevel() const
	{
		return m_level;
	}
	ATTR_INLINE
	void setLevel(LogLevel level)
	{
		m_level = level;
	}
	ATTR_INLINE
	void setFormatter(LogFormatter::ptr formatter)
	{
		WriteLockGuard lock(m_rwlock);
		m_format = std::move(formatter);
	}
	void setFormatter(const std::string& pattern)
	{
		auto format = std::make_shared<LogFormatter>(pattern);
		WriteLockGuard lock(m_rwlock);
		m_format = format;
	}

	ATTR_PURE_INLINE
	LogFormatter::ptr getFormatter() const
	{
		ReadLockGuard lock(m_rwlock);
		return m_format;
	}

	void addAppender(const LogAppender::ptr& appender);
	void addAppender(LogAppender::ptr&& appender);

	void delAppender(const LogAppender::ptr& appender);

 private:
	void logging(std::stringstream&& logStream, LogLevel level);

	std::string m_name;
	LogLevel m_level;
	/// @brief 格式化器
	LogFormatter::ptr m_format;
	std::vector<LogAppender::ptr> m_appenders;
	mutable RWLock m_rwlock;
};

class LoggerManager : noncopyable
{
 public:
	using LoggerMap = std::unordered_map<std::string, Logger::unique_ptr>;

	static LoggerManager* getInstance() noexcept
	{
		return Singleton<LoggerManager>::getInstance();
	}

	/// @brief 获取Logger对象
	/// @param loggerName log name
	/// @return Logger
	//TODO:需要处理配置文件中无当前配置但代码中获取了的情况
	Logger& getLogger(const std::string& loggerName);

	Logger& getRoot() noexcept;

	/// @brief 载入logger配置
	/// @param name 配置文件名
	/// @attend ConfigManager须先载入此配置文件
	void LoadConfig(std::string_view name) noexcept;

 private:
	MutexLock m_mutex;
	LoggerMap m_loggers;
};

template<typename T, typename =typename std::enable_if_t<std::is_pointer<T*>::value>>
constexpr T* CheckNotNull(const char* file, int line, std::string_view func, std::string_view log, T* val)
{
	if (!static_cast<bool>(val))
	{
		Logging(LOG_ROOT, file, line, func.data(), LogLevel::FATAL).getStream() << log;
	}
	return val;
}

template<typename T>
constexpr T LogThrow(const char* file, int line, const char* func, T&& ex)
{
	Logging(LOG_ROOT, file, line, func, LogLevel::FATAL).getStream() << "throw exception,reason:" << ex.what();
	return ex;
}

template<typename T>
constexpr T AssertFail(const char* expr, const char* file, int line, const char* func, T&& ex)
{
	Logging(LOG_ROOT, file, line, func, LogLevel::FATAL).getStream() << "error expression:(" << expr << "),reason:"
																	 << ex.what();
	return ex;
}
}