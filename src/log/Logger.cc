#include <log/Logger.h>
#include <exception/Exception.hpp>
#include <config/config.h>
#include <base/Macro.h>
#include <util/StringUtil.hpp>
#include <memory>
#include <algorithm>
#include <utility>

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
:m_name(std::move(name)),m_level(LogLevel::DEBUG),
m_format(nullptr)
{

}

//Logger::Logger(const std::string& name)
//:m_name(name),m_level(LogLevel::DEBUG),
//m_format(nullptr)
//{
//
//}

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
    if(!logConfig.has_value())
    {
        return;
    }
    for(auto& node:logConfig.value())
    {
        auto name=node.find("Name")->second->asString();
        ASSERT(name.has_value(),"logger name must be configure,please check the configuration file");
		auto logPath=node.find("Path")->second->asString().value_or("log");
		bool EnableStdout=node.find("EnableStdout")->second->asBoolean().value_or(true);
		auto format=node.find("Format")->second->asString().value_or("%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");
		auto action=node.find("Action")->second->asString().value_or("SYNC");
        auto flushInterval=node.find("FlushInterval")->second->asInteger().value_or(3);
        auto flushLogCount=node.find("FlushLogCount")->second->asInteger().value_or(1024);
        auto singleFileSize=node.find("SingleFileSize")->second->asString().value_or("64MB");
        auto logger=std::make_unique<Logger>(name.value().data());
        logger->setFormatter(format);
        LogAppender::ptr appender=std::make_shared<FileLogAppender>(name.value(),logPath,
                        action=="ASYNC"?AppenderAction::ASYNC:AppenderAction::SYNC,flushInterval,flushLogCount,StringUtil::StrToFileSize(singleFileSize.c_str()));
        logger->addAppender(appender);
        if(EnableStdout)
        {
            logger->addAppender(std::make_shared<StdoutLogAppender>());
        }

        {
            MutexLockGuard lock(m_mutex);
            m_loggers.insert(std::make_pair(name.value(),std::move(logger)));
        }
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
