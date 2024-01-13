#pragma once
#include <cstddef>
#include <base/Type.hpp>

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


constexpr off64_t StrToFileSize(const char* str)
{
    off64_t size=0;
    while(str)
    {
        if(*str>='0' && *str<='9')
        {
            size = size*10+(*str-'0');
        }
        else
        {
            if(*(str+1)!='B')
            {
                return size;
            }
            switch(*str)
            {
                case 'M':
                    return operator""_MB(size);
                    break;
                case 'G':
                    return operator""_GB(size);
                    break;
                case 'T':
                    return operator""_TB(size);
                    break;
                default:
                    return size;
            }
        }
        ++str;

    }

    return size;
}


}
