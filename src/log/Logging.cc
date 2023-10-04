#include <log/Logging.h>
#include <time/Timestamp.h>
#include <thread/CurrentThread.h>
#include <log/Logger.h>
#include <iostream>

LogContext::LogContext(const SourceFile &file, int32_t line, std::string&& funcName,time_t elapse, pid_t threadId, uint32_t fiberId,
                       Timestamp time, const std::string &threadName, LogLevel level,const std::string& loggerName )
                       :m_file(file),m_line(line),m_funcName(funcName),m_elapse(elapse),m_threadId(threadId),m_fiberId(fiberId),m_time(time),
                       m_threadName(threadName),m_level(level),m_loggerName(loggerName)
{
}
Loggin::Loggin(const Logger& logger,SourceFile file, int32_t line, std::string&& func, LogLevel level)
:m_logger(logger),
m_context(file,line,std::move(func),0,CurrentThread::tid(),0,Timestamp::now(),CurrentThread::ThreadName(),level,m_logger.getName())
{

}
Loggin::~Loggin()
{
    getStream()<<"Hello";
    std::stringstream stream=m_logger.getFormatter()->Format(m_context);
    std::cout<<stream.str()<<std::endl;
}
