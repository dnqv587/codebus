#pragma once
#include <cstddef>

namespace StringUtil
{
/// @brief 解析文件名
template<std::size_t N>
constexpr const char* BaseFileName(const char (&filename)[N])
{
    for(auto i=N;i>0;--i)
    {
        if(filename[i-1]=='/')
        {
            return filename+i;
        }
    }
    return filename;
}


}
