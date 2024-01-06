#include <log/Logger.h>
#include <exception/Exception.hpp>
#include <config/config.h>
#include <memory>
#include <algorithm>

namespace
{
namespace util
{
Logger::unique_ptr getRootLogger()
{
	Logger::unique_ptr logger=std::make_unique<Logger>("Root");
	FileLogAppender::ptr fileAppender = std::make_shared<FileLogAppender>("Root","log",AppenderAction::SYNC,3,1024,10_MB);
	StdoutLogAppender::ptr stdAppender = std::make_shared<StdoutLogAppender>();
	logger->setFormatter("%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");
	logger->setLevel(LogLevel::TRACE);
	logger->addAppender(fileAppender);
	logger->addAppender(stdAppender);
	return logger;
}

}
}

Logger::Logger(std::string &&name)
:m_name(name),m_level(LogLevel::DEBUG),
m_format(nullptr)
{

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

Logger& LoggerManager::getLogger(const std::string &loggerName)
{
    MutexLockGuard lock(m_mutex);
    auto log=m_loggers.find(loggerName);
    if(log == m_loggers.end())
    {
		LOG_THROW(Exception::LoggerError("non getting logger,please check logger name!"));
    }
    return *log->second;
}
void LoggerManager::LoadConfig(std::string_view configName) noexcept
{
	auto configMan=ConfigManager::getInstance();

	auto config= configMan->getConfig(configName.data());
	if(!config.has_value())
    {
		return;
	}
	auto logConfig=config.value().get().getConfig("Logger").asArrayTable();
    for(auto& node:logConfig)
    {
        auto name=node.find("Name")->second->asString();
		auto logPath=node.find("Path")->second->asString();
		bool EnableStdout=node.find("EnableStdout")->second->asBoolean();
		auto formatter=node.find("Format")->second->asString();
		auto action=node.find("Action")->second->asString();
		CreateLogger(name,logPath,formatter,action=="ASYNC"?AppenderAction::ASYNC:AppenderAction::SYNC,EnableStdout);
    }

}

void LoggerManager::CreateLogger(std::string_view name, std::string_view path, std::string_view format,AppenderAction action,bool EnableStdout)
{
	auto logger=std::make_unique<Logger>(name.data());
	logger->setFormatter(format.data());
	LogAppender::ptr appender=std::make_shared<FileLogAppender>(name.data(),path.data(),action);
	logger->addAppender(appender);
	if(EnableStdout)
	{
		logger->addAppender(std::make_shared<StdoutLogAppender>());
	}

	{
		MutexLockGuard lock(m_mutex);
		m_loggers.insert(std::make_pair(name,std::move(logger)));
	}
}

Logger &LoggerManager::getRoot() noexcept
{
    MutexLockGuard lock(m_mutex);
    auto root=m_loggers.find("Root");
    if(root!=m_loggers.end())
    {
        return *root->second;
    }

    m_loggers.insert(std::make_pair("Root",util::getRootLogger()));
    return *m_loggers.at("Root");
}
