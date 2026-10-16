//***********************************************
// name      : config.h
// brief     : 配置模块抽象接口定义
// attention : None
//***********************************************
#pragma once
#include <base/noncopyable.hpp>
#include <thread/Mutex.hpp>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cstdint>
#include <chrono>
#include <string_view>
#include <ostream>
#include <filesystem>
#include <shared_mutex>

namespace bus {

/// @brief 获取系统配置
#define SYS_CONF(XX) sys_config.get(XX)

/// @brief 配置值抽象基类，不支持拷贝，只支持移动
class Value
{
 public:
    using ptr = std::shared_ptr<Value>;
    using weak_ptr = std::weak_ptr<Value>;

    using table_t = std::unordered_map<std::string_view, Value::ptr>;
    using array_t = std::vector<Value::ptr>;

    using DateTime = std::chrono::sys_time<std::chrono::microseconds>;
    using Time = std::chrono::hh_mm_ss<std::chrono::microseconds>;
    using Date = std::chrono::year_month_day;

    Value() = default;
    virtual ~Value() = default;

    /// @brief 类型检查方法（纯虚函数）
    ATTR_PURE_INLINE
    virtual bool isString() const noexcept = 0;
    ATTR_PURE_INLINE
    virtual bool isInteger() const noexcept = 0;
    ATTR_PURE_INLINE
    virtual bool isFloat() const noexcept = 0;
    ATTR_PURE_INLINE
    virtual bool isBoolean() const noexcept = 0;
    ATTR_PURE_INLINE
    virtual bool isDate() const noexcept = 0;
    ATTR_PURE_INLINE
    virtual bool isTime() const noexcept = 0;
    ATTR_PURE_INLINE
    virtual bool isDateTime() const noexcept = 0;
    ATTR_PURE_INLINE
    virtual bool isArray() const noexcept = 0;
    ATTR_PURE_INLINE
    virtual bool isTable() const noexcept = 0;

    /// @brief 类型转换方法（纯虚函数，返回 std::optional）
    ATTR_NODISCARD
    virtual std::optional<std::string_view> asString() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<std::int64_t> asInteger() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<double> asFloat() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<bool> asBoolean() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<Date> asDate() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<Time> asTime() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<DateTime> asDateTime() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<array_t> asArray() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<table_t> asTable() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<std::vector<table_t>> asArrayTable() const noexcept = 0;
    ATTR_NODISCARD
    virtual std::optional<Value::ptr> seek(std::string_view config_name) const noexcept = 0;

    friend std::ostream& operator<<(std::ostream& lhs, const Value& rhs)
    {
        return rhs.print_value(lhs);
    }

 protected:
    ATTR_PURE_INLINE
    virtual std::ostream& print_value(std::ostream& os) const noexcept = 0;
};


enum class ConfigType{
    TOML
};

class Handle;
class Config : noncopyable
{
 public:
    explicit Config(ConfigType type = ConfigType::TOML) noexcept
    :m_type(type),
    m_isLoaded(false)
    {}
    
    ~Config() = default;

    /// @brief 从文件加载配置
    /// @param file 配置文件路径
    /// @throw Exception::ConfigError 文件打开或解析失败
    void LoadFile(std::filesystem::path file);

    /// @brief 从字符串加载配置
    /// @param doc TOML 格式的字符串
    /// @throw Exception::ConfigError 解析失败
    void Load(std::string_view doc);

    /// @brief 获取配置值
    /// @param config_name 配置键路径（支持点号分隔，如 "server.port"）
    /// @return shared_ptr 指向的 value 对象，不存在时返回 NullValue 的单例
    /// @throw Exception::ConfigUnLoad 未加载配置
    ATTR_NODISCARD
    Value::ptr get(std::string_view config_name);

    ATTR_NODISCARD
    bool isLoaded() const noexcept{
        return m_isLoaded;
    }

 private:
    ConfigType m_type;
    std::shared_ptr<Handle> m_handle;
    /// @brief 是否已加载配置
    bool m_isLoaded;
    //MutexLock m_mutex;
    std::shared_mutex m_mutex;
};

inline Config sys_config;

} // namespace bus
