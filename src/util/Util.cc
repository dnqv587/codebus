#include "Util.h"
#include <execinfo.h>
#include "base/type.h"
#include "log/Logger.h"

namespace uf
{
static Logger& g_logger = GET_LOGGER("system");

/// @brief 进程启动时间
static Timestamp ProcessRunTime = Timestamp::now();

Timestamp TimeUtil::getElapse()
{
	return Timestamp::now() - ProcessRunTime;
}

off64_t StringUtil::StrToFileSize(const char* str)
{
	off64_t size = 0;
	while (str)
	{
		if (*str >= '0' && *str <= '9')
		{
			size = size * 10 + (*str - '0');
		}
		else
		{
			if (*(str + 1) != 'B')
			{
				return size;
			}
			switch (str[0])
			{
			case 'M':
				return operator ""_MB(size);
				break;
			case 'G':
				return operator ""_GB(size);
				break;
			case 'T':
				return operator ""_TB(size);
				break;
			default:
				return size;
			}
		}
		++str;

	}

	return size;
}

std::vector<std::string> SystemUtil::Backtrace(int size, int skip)
{
	std::vector<std::string> bt;
	void** buffer = (void**)malloc(sizeof(void*) * size);
	int s = ::backtrace(buffer, size);
	bt.reserve(s - skip);

	char** strings = ::backtrace_symbols(buffer, s);
	for (int i = skip; i < s; ++i)
	{
		bt.emplace_back(strings[i]);
	}
	free(buffer);
	free(strings);

	return bt;
}

std::string SystemUtil::BacktraceToString(int size, int skip, std::string_view prefix)
{
	auto bt = Backtrace(size, skip);
	std::stringstream ss;
	for (const auto& i : bt)
	{
		ss << prefix << i << std::endl;
	}
	return ss.str();
}

void SystemUtil::Abort()
{
	LOG_FATAL(g_logger) << "the system will exit soon, Backtrace: " << BacktraceToString(64, 3);
	abort();
}

}