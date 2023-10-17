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
    using LogStreamPtr=std::shared_ptr<std::stringstream>;

    explicit LogFormatter(std::string&& pattern);
    explicit LogFormatter(const std::string& pattern);
    ~LogFormatter()=default;
    /// @brief 格式化日志
    /// @param context 日志上下文
    /// @return 日志流
    LogStreamPtr Format(const LogContext& context);

    class FormatItem
    {
    public:
        using ptr=std::shared_ptr<FormatItem>;
        virtual ~FormatItem()=default;

        virtual void Format(std::stringstream& stream,const LogContext& context,std::string&& suffix,std::string&& TimeFMT)=0;

    };

    class LogPattern
    {
    public:
        // %xx + xxxxx
        using Vec_Type=std::function<void (std::stringstream&,const LogContext& )>;

        explicit LogPattern(std::string&& pattern);
        explicit LogPattern(const std::string& pattern);

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
     *  %h hostname主机名
     *  %P 进程号
     */
    std::unique_ptr<LogPattern> m_pattern;
};
