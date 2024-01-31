#pragma once
#include <base/noncopyable.hpp>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <map>

class LogContext;
class  LogFormatter:noncopyable
{
public:
    using ptr=std::shared_ptr<LogFormatter>;

    explicit LogFormatter(std::string&& pattern);
    explicit LogFormatter(const std::string& pattern);
    ~LogFormatter()=default;
    /// @brief 格式化日志
    /// @param context 日志上下文
    /// @return 日志流
    std::stringstream Format(LogContext& context);

    class FormatItem
    {
    public:
        using ptr=std::shared_ptr<FormatItem>;
        using unique_ptr=std::shared_ptr<FormatItem>;

        virtual ~FormatItem()=default;

        virtual void Format(std::stringstream& stream, LogContext& context,std::string&& suffix,std::string&& TimeFMT)=0;

    };

    class LogPattern
    {
    public:
        // %xx + xxxxx
        using Vec_Type=std::function<void (std::stringstream&,LogContext& )>;

        explicit LogPattern(std::string&& pattern);
        explicit LogPattern(const std::string& pattern);

        void Init();

        [[nodiscard]]
		const std::vector<Vec_Type>& getLogItem() const
        {return m_ItemVec;}

    private:
        std::string m_pattern;
        static std::unordered_map<char,LogFormatter::FormatItem::unique_ptr> s_ItemMap;
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
