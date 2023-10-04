#include <log/Logger.h>
#include <memory>

/// @brief 默认格式模板
static std::string g_defaultPattern="%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n";

Logger::Logger(std::string &&name)
:m_name(name),m_level(LogLevel::DEBUG),
m_format(std::make_shared<LogFormatter>(g_defaultPattern))
{

}

Logger &Logger::getRoot() const
{
    static Logger logger("Root");
    return logger;
}
