#pragma once 
#include <cstring>
#include <cstdint>
#include <string>

//************************************
// class:  SourceFile
// brief:  Դ�ļ���������
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
	const char* m_data;//SourceFile����
	size_t m_size;
};

enum class LogLevel
{
	// TRACE ����
	TRACE ,
	// DEBUG ����
	DEBUG ,
	// INFO ����
	INFO ,
	// WARN ����
	WARN ,
	// ERROR ����
	ERROR ,
	// FATAL ����
	FATAL ,
	
	NUM_LOG_LEVELS

};

class LogConText
{
public:

private:
	//�ļ���
	SourceFile m_file;
	//�к�
	int32_t m_line;
	//��������ʱ��--����
	uint32_t  m_elapse;
	//�̺߳�
	uint32_t m_threadId;
	//Э�̺�
	uint32_t m_fiberId;
	//ʱ��
	Timestamp m_time;
	//�߳���
	std::string m_threadName;
	//��־�ȼ�
	LogLevel m_level;
	//��־��
	std::stringstream& m_stream;
};

