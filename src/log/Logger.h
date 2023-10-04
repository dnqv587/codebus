#pragma once
#include <log/Logging.h>
#include <base/noncopyable.hpp>
#include <log/LogFormatter.h>
#include <thread/Mutex.hpp>

class Logger:noncopyable
{
public:
    Logger(std::string&& name);

	const std::string& getName() const
	{return m_name;}

	const LogLevel getLevel() const
	{return m_level;}

	void setLevel(LogLevel level)
	{m_level=level;}

	void setFormatter(const LogFormatter::ptr formatter)
    {m_format=formatter;}
	void setFormatter(const std::string &pattern)
    {m_format = std::make_shared<LogFormatter>(pattern);}

    LogFormatter::ptr getFormatter() const {return m_format;}

    Logger& getRoot() const;
private:
    std::string m_name;
	LogLevel m_level;
	LogFormatter::ptr m_format;
	MutexLock m_mutex;
};
