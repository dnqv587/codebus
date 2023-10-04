#pragma once
#include <base/noncopyable.hpp>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <map>

class LogContext;
class LogPattern;

class LogFormatter:noncopyable
{
public:
    using ptr=std::shared_ptr<LogFormatter>;

    LogFormatter(std::string&& pattern);
    LogFormatter(const std::string& pattern);
    ~LogFormatter();
    /// @brief 格式化日志
    /// @param context 日志上下文
    /// @return 日志流
    std::stringstream Format(const LogContext& context);

    class FormatItem
    {
    public:
        using ptr=std::shared_ptr<FormatItem>;
        virtual ~FormatItem(){}

        virtual void Format(std::stringstream& stream,const LogContext& context,std::string&& suffix="",std::string&& TimeFMT="")=0;

    };

    class LogPattern
    {
    public:
        // %xx + xxxxx
        using Vec_Type=std::function<void (std::stringstream&,const LogContext& )>;

        LogPattern(std::string&& pattern);
        LogPattern(const std::string& pattern);

        void Init();

        const std::vector<Vec_Type>& getLogItem() const
        {return m_ItemVec;}

    private:
        std::string m_pattern;
        static std::map<char,LogFormatter::FormatItem::ptr> s_ItemMap;
        std::vector<Vec_Type> m_ItemVec;
    };

private:
    /**
     * @brief 格式模板
     *  %m 消息
     *  %p 日志级别
     *  %r 累计毫秒数
     *  %c 日志名称
     *  %t 线程id
     *  %n 换行
     *  %d 时间 {%Y-%m-%d %H:%M:%S.%l}
     *  %f 文件名
     *  %l 行号
     *  %M 函数名
     *  %T 制表符
     *  %F 协程id
     *  %N 线程名称
     *  %% "%"字符
     *m_pattern = {std::unique_ptr<LogFormatter::LogPattern>}
     *  默认格式 "%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n"
     */
    std::unique_ptr<LogPattern> m_pattern;
};
