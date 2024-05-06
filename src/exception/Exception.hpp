#pragma once
#include <exception>
#include <stdexcept>


namespace uf::Exception
{
class ConfigUnLoad : public std::exception
{
public:
    ConfigUnLoad() noexcept: m_what("config unload")
    {}

    [[nodiscard]] const char *what() const noexcept override
    {
        return m_what;
    }

protected:
    explicit ConfigUnLoad(const char *what) noexcept: m_what(what)
    {}

private:
    const char *const m_what;
};

/// @brief DataTime格式错误
class DateTimeFormatError : public std::logic_error
{
public:
    DateTimeFormatError() noexcept
    :std::logic_error("DateTime Format Error")//,m_what("DateTime Format Error")
    {}

    explicit DateTimeFormatError(const char* what) noexcept
            :logic_error(what)//,m_what{what}
            {}

//    [[nodiscard]] const char* what() const noexcept override
//    {
//        return m_what;
//    }
private:
   // const char *const m_what;
};


class ConfigError : public std::logic_error
{
public:
    ConfigError() noexcept
            :std::logic_error("Config Error")
    {}

    explicit ConfigError(const char* what) noexcept
            :logic_error(what)
    {}

};

class LoggerError : public std::logic_error
{
 public:
	LoggerError() noexcept
		:std::logic_error("ConfigError Error")
	{}

	explicit LoggerError(const char* what) noexcept
		:logic_error(what)
	{}

};

}
