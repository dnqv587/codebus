#pragma once
#include <vector>
#include "base/Macro.h"
#include "time/Timestamp.h"

namespace bus
{
namespace TimeUtil
{
/// @brief 获取程序运行时间
/// @return
ATTR_NODISCARD
Timestamp getElapse();

}

namespace StringUtil
{
/// @brief 解析文件名
template<std::size_t N>
constexpr const char* BaseFileName(const char (& filename)[N])
{
	for (auto i = N; i > 0; --i)
	{
		if (filename[i - 1] == '/')
		{
			return filename + i;
		}
	}
	return filename;
}

off64_t StrToFileSize(const char* str);

}

namespace SystemUtil
{
/// @brief 获取函数调用栈信息
/// @param size 调用栈信息个数
/// @param skip 跳过调用栈的个数
/// @return 调用栈信息
ATTR_NODISCARD
std::vector<std::string> Backtrace(int size, int skip);

std::string BacktraceToString(int size, int skip, std::string_view prefix = "");

void Abort();
}

}

