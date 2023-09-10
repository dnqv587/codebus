#pragma once 
#include <cstring>
#include <cstdint>
#include <string>

//************************************
// class:  SourceFile
// brief:  源文件名处理类
//************************************
class SourceFile
{
public:
	template <size_t n>
	SourceFile(const char(&arr)[N])
		:m_data(arr)
	{
		const char* slash = ::strrchr(m_data, '/');
		if (slash)
		{
			m_data = slash + 1;
			m_size -= m_data - arr;
		}
	}

	SourceFile(const char* FileName)
		:m_data(FileName)
	{
		const char* slash = ::strrchr(m_data, '/');
		if (slash)
		{
			m_data = slash + 1;
			m_size = ::strlen(m_data);
		}
	}

	const char* FileName() const 
	{
		return m_data;
	}

	size_t size() const
	{
		return m_size;
	}

private:
	const char* m_data;//SourceFile名称
	size_t m_size;
};

enum class LogLevel
{
	// TRACE 级别
	TRACE ,
	// DEBUG 级别
	DEBUG ,
	// INFO 级别
	INFO ,
	// WARN 级别
	WARN ,
	// ERROR 级别
	ERROR ,
	// FATAL 级别
	FATAL ,
	
	NUM_LOG_LEVELS

};

class LogConText
{
public:

private:
	//文件名
	SourceFile m_file;
	//行号
	int32_t m_line;
	//程序启动时间--毫秒
	uint32_t  m_elapse;
	//线程号
	uint32_t m_threadId;
	//协程号
	uint32_t m_fiberId;
	//时间
	Timestamp m_time;
	//线程名
	std::string m_threadName;
	//日志等级
	LogLevel m_level;
	//日志流
	std::stringstream& m_stream;
};

