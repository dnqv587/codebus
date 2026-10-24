#include "log/Logger.h"
#include <memory>
#include <algorithm>
#include <utility>
#include "Exception.hpp"
#include "Config.h"
#include "Util.h"

namespace bus
{

namespace util
{
Logger::unique_ptr getRootLogger()
{
	Logger::unique_ptr logger = std::make_unique<Logger>("Root");
	FileLogAppender::ptr
		fileAppender = std::make_shared<FileLogAppender>("Root", "log", AppenderAction::SYNC, 3, 1024, 10_MB);
	StdoutLogAppender::ptr stdAppender = std::make_shared<StdoutLogAppender>();
	logger->setFormatter("%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");
	logger->setLevel(LogLevel::TRACE);
	logger->addAppender(fileAppender);
	logger->addAppender(stdAppender);
	return logger;
}

}

static std::unordered_map<std::string_view, LogLevel> LogLevelPair = {
#define XX(level) \
           {std::make_pair(#level,LogLevel::level)}

	XX(TRACE),
	XX(DEBUG),
	XX(INFO),
	XX(WARN),
	XX(ERROR),
	XX(FATAL)

#undef XX
};

Logger::Logger(std::string&& name, LogLevel level)
	: m_name(std::move(name)), m_level(level),
	  m_format(nullptr)
{

}

//Logger::Logger(const std::string& name)
//:m_name(name),m_level(LogLevel::DEBUG),
//m_format(nullptr)
//{
//
//}

void Logger::addAppender(LogAppender::ptr&& appender)
{
	WriteLockGuard lock(m_rwlock);
	m_appenders.push_back(std::forward<LogAppender::ptr>(appender));
}

void Logger::addAppender(const LogAppender::ptr& appender)
{
	WriteLockGuard lock(m_rwlock);
	m_appenders.push_back(appender);
}

void Logger::delAppender(const LogAppender::ptr& appender)
{
	WriteLockGuard lock(m_rwlock);
	auto iter = std::find(m_appenders.begin(), m_appenders.end(), appender);
	if (iter != m_appenders.cend())
	{
		m_appenders.erase(iter);
	}
}

void Logger::logging(std::stringstream&& logStream, LogLevel level)
{
	std::string log = logStream.str();
	ReadLockGuard lock(m_rwlock);
	for (auto& appender : m_appenders)
	{
		appender->append(log, level);
	}
}

Logger::Logger(const std::string& name, LogLevel level)
	: m_name(name), m_level(level),
	  m_format(nullptr)
{
}

Logger& LoggerManager::getLogger(const std::string& loggerName)
{
	MutexLockGuard lock(m_mutex);
	auto log = m_loggers.find(loggerName);
	if (log != m_loggers.end())
	{
		return *log->second;
	}
	auto [iter,flag] = m_loggers.emplace(std::make_pair(loggerName,std::make_unique<Logger>(loggerName)));
	if(!flag)
	{
		throw Exception::LoggerError("LoggerManager::getLogger insert error!");
	}
	return *iter->second;
}

void LoggerManager::LoadConfig(std::string_view configName) noexcept
{
	auto logConfig = SYS_CONF("logger")->asArray();
	if (!logConfig.has_value())
	{
		return;
	}
	for (auto& node : logConfig.value())
	{
		auto name = node->seek("name").value()->asString().value_or("unnamed");
		auto logPath = node->seek("path").value()->asString().value_or("log");
		bool EnableStdout = node->seek("EnableStdout").value()->asBoolean().value_or(true);
		auto format =
			node->seek("format").value()->asString().value_or("%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");
		auto action = node->seek("action").value()->asString().value_or("SYNC");
		auto flushInterval = node->seek("FlushInterval").value()->asInteger().value_or(3);
		auto flushLogCount = node->seek("FlushLogCount").value()->asInteger().value_or(1024);
		auto singleFileSize = node->seek("SingleFileSize").value()->asString().value_or("64MB");
		auto levelIter = LogLevelPair.find(node->seek("level").value()->asString().value_or("DEBUG"));
		LogLevel level = LogLevel::DEBUG;
		if (levelIter != LogLevelPair.end())
		{
			level = levelIter->second;
		}

		LogAppender::ptr appender = std::make_shared<FileLogAppender>(name.data(),
			logPath.data(),
			action == "ASYNC" ? AppenderAction::ASYNC : AppenderAction::SYNC,
			flushInterval,
			flushLogCount,
			StringUtil::StrToFileSize(singleFileSize.data()));

		LogAppender::ptr stdAppender;
		if (EnableStdout)
		{
			stdAppender = std::make_shared<StdoutLogAppender>();
		}

		auto iter = m_loggers.find(name.data());
		if(iter != m_loggers.cend())
		{
			MutexLockGuard lock(m_mutex);
			auto& log = iter->second;
            log->setLevel(level);
			log->setFormatter(format.data());
			log->addAppender(std::move(appender));
			if (EnableStdout)
			{
				log->addAppender(std::move(stdAppender));
			}
		}
		else
		{
			auto logger = std::make_unique<Logger>(name.data(), level);
			logger->setFormatter(format.data());

			logger->addAppender(std::move(appender));
			if (EnableStdout)
			{
				logger->addAppender(std::move(stdAppender));
			}

			{
				MutexLockGuard lock(m_mutex);
				m_loggers.insert(std::make_pair(name, std::move(logger)));
			}
		}
	}
}

// Logger& LoggerManager::getRoot() noexcept
// {
// 	MutexLockGuard lock(m_mutex);
// 	auto root = m_loggers.find("Root");
// 	if (root != m_loggers.end())
// 	{
// 		return *root->second;
// 	}

// 	m_loggers.insert(std::make_pair("Root", util::getRootLogger()));
// 	return *m_loggers.at("Root");
// }

}