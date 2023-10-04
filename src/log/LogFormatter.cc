#include <log/LogFormatter.h>
#include <log/Logging.h>
#include <utility>
#include <map>
#include <vector>
#include <algorithm>
#include <functional>
#include <iostream>

static_assert(static_cast<unsigned long>(LogLevel::NUM_LOG_LEVELS) == 6,"LogLevel size invalid");
constexpr const char* LogLevelName[static_cast<unsigned long>(LogLevel::NUM_LOG_LEVELS)]=
        {
            "TRACE",
            "DEBUG",
            "INFO",
            "WARN",
            "ERROR",
            "FATAL"
        };

/// @brief 默认时间格式
static std::string g_defaultTimeFormat="%Y-%m-%d %H:%M:%S";
/// @brief 默认格式模板
static std::string g_defaultPattern="%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n";


#define FORMATITEM
//%f 文件名
#define ITEM_NAME  FileFormatItem
#define ITEM_BODY \
    OUTPUT(context.getSourceFile().FileName())
#include <base/MacroReflaction.hpp>
//%l 行号
#define ITEM_NAME  LineFormatItem
#define ITEM_BODY \
    OUTPUT(context.getLine())
#include <base/MacroReflaction.hpp>
//%r 累计时间[ms]
#define ITEM_NAME  ElapseFormatItem
#define ITEM_BODY \
    OUTPUT(context.getElapse())
#include <base/MacroReflaction.hpp>
//%t 线程id
#define ITEM_NAME  ThreadIdFormatItem
#define ITEM_BODY \
    OUTPUT(context.getThreadId())
#include <base/MacroReflaction.hpp>
//%F 协程号
#define ITEM_NAME  FiberIdFormatItem
#define ITEM_BODY \
    OUTPUT(context.getFiberId())
#include <base/MacroReflaction.hpp>
//%d 时间
#define ITEM_NAME  TimeFormatItem
#define ITEM_BODY \
    OUTPUT(context.getTime().toFormatString(std::move(TimeFMT)))
#include <base/MacroReflaction.hpp>
//%N 线程名
#define ITEM_NAME  ThreadNameFormatItem
#define ITEM_BODY \
    OUTPUT(context.getThreadName())
#include <base/MacroReflaction.hpp>
//%p 日志级别
#define ITEM_NAME  LevelFormatItem
#define ITEM_BODY \
    OUTPUT(LogLevelName[static_cast<unsigned long>(context.getLogLevel())])
#include <base/MacroReflaction.hpp>
//%m 消息
#define ITEM_NAME  MassageFormatItem
#define ITEM_BODY \
    OUTPUT(context.getStream().str())
#include <base/MacroReflaction.hpp>
//%c 日志名称
#define ITEM_NAME  LogNameFormatItem
#define ITEM_BODY \
    OUTPUT(context.getLoggerName())
#include <base/MacroReflaction.hpp>
//%n 换行
#define ITEM_NAME  EnterFormatItem
#define ITEM_BODY \
    OUTPUT('\n')
#include <base/MacroReflaction.hpp>
//%M 函数名
#define ITEM_NAME  FunctionNameFormatItem
#define ITEM_BODY \
    OUTPUT(context.getFuncName())
#include <base/MacroReflaction.hpp>
//%T 制表符
#define ITEM_NAME  TabFormatItem
#define ITEM_BODY \
    OUTPUT('\t')
#include <base/MacroReflaction.hpp>
//%% %字符
#define ITEM_NAME  PercentFormatItem
#define ITEM_BODY \
    OUTPUT('%')
#include <base/MacroReflaction.hpp>

#define ITEM_NAME  NoneFormatItem
#define ITEM_BODY \
    OUTPUT("")
#include <base/MacroReflaction.hpp>
#undef FORMATITEM



std::map<char,LogFormatter::FormatItem::ptr> LogFormatter::LogPattern::s_ItemMap
{
#define XX(symbol,obj) \
        {*(#symbol),std::make_shared<obj>()}

        XX(f, FileFormatItem),               //f:文件名
        XX(l, LineFormatItem),               //l:行号
        XX(r, ElapseFormatItem),             //r:累计毫秒数
        XX(t, ThreadIdFormatItem),           //t:线程id
        XX(F, FiberIdFormatItem),            //F:协程id
        XX(d, TimeFormatItem),               //d:时间
        XX(N, ThreadNameFormatItem),         //N:线程名称
        XX(p, LevelFormatItem),              //p:日志级别
        XX(m, MassageFormatItem),            //m:消息
        XX(c, LogNameFormatItem),            //c:日志名称
        XX(n, EnterFormatItem),              //n:换行
        XX(M, FunctionNameFormatItem),       //M:函数名
        XX(T, TabFormatItem),                //T:Tab
        XX(%, PercentFormatItem),            //%:%字符
        XX(0, NoneFormatItem)               //0:空字符
#undef XX
 };

void LogFormatter::LogPattern::Init()
{
    //子串头
    std::string::const_iterator begin=m_pattern.cbegin();
    //子串尾
    std::string::const_iterator end=m_pattern.cbegin();
    //符号标志
    char symbol='0';
    while(begin!=m_pattern.cend())
    {
        std::string suffix;
        std::string TimeFMT;
        //取时间格式子串
        if('d' == symbol)
        {
            if(begin != m_pattern.cend()&&*begin == '{')
            {
                auto result=std::find(begin,m_pattern.cend(),'}');
                if(result != m_pattern.cend())
                {
                    TimeFMT.assign(begin+1,result);
                    begin=result+1;
                }

            }
        }
        //取后缀子串
        end=std::find(begin,m_pattern.cend(),'%');
        suffix.assign(begin, end);
        auto iter = s_ItemMap.find(symbol);
        if (iter == s_ItemMap.cend())
        {
            iter = s_ItemMap.find('0');
        }
        //格式化函数容器
        m_ItemVec.push_back([suffix = std::move(suffix), iter, TimeFMT = std::move(TimeFMT)](std::stringstream &stream,
                                                                                             const LogContext &context)mutable -> void {
            iter->second->Format(stream, context, std::move(suffix), std::move(TimeFMT));
        });
        if (end != m_pattern.cend() && end + 1 != m_pattern.cend())
        {
            symbol = *(end + 1);
            end = end + 2;
        } else
        {
            symbol = '0';
            end = m_pattern.cend();
        }

        begin = end;
    }
}

LogFormatter::LogPattern::LogPattern(std::string &&pattern)
:m_pattern(pattern.empty()?g_defaultPattern:std::move(pattern))
{
    Init();
}

LogFormatter::LogPattern::LogPattern(const std::string &pattern)
:m_pattern(pattern.empty()?g_defaultPattern:pattern)
{
    Init();
}

LogFormatter::LogFormatter(std::string &&pattern)
:m_pattern(std::make_unique<LogPattern>(std::move(pattern)))
{

}

LogFormatter::LogFormatter(const std::string &pattern)
:m_pattern(std::make_unique<LogPattern>(pattern))
{

}

LogFormatter::~LogFormatter()
{

}

std::stringstream LogFormatter::Format(const LogContext &context)
{
    std::stringstream stream;
    auto ItemVec = m_pattern->getLogItem();
    for (const auto &func: ItemVec)
    {
        func(stream, context);
    }
    return stream;
}








