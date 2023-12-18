#pragma once
#include <base/noncopyable.hpp>
#include <base/copyable.hpp>
#include <thread/Mutex.hpp>
#include <toml++/toml.hpp>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <iostream>

class Date{};
class Time{};
class DateTime{};
class Value:copyable
{
 public:
    using ptr=std::shared_ptr<Value>;
	using uniqe_ptr=std::unique_ptr<Value>;

    Value()=default;
    virtual ~Value()=default;

    virtual bool isString()const noexcept   =0;
    virtual bool isInteger()const noexcept  =0;
    virtual bool isFloat()const noexcept    =0;
    virtual bool isBoolean()const noexcept  =0;
    virtual bool isDate()const noexcept     =0;
    virtual bool isTime()const noexcept     =0;
    virtual bool isDateTime()const noexcept =0;
    virtual bool isArray() const noexcept   =0;
    virtual bool isTable() const noexcept   =0;

    [[nodiscard]] virtual std::string asString()const noexcept   =0;
    [[nodiscard]] virtual std::int64_t asInteger()const noexcept =0;
    [[nodiscard]] virtual double asFloat()const noexcept         =0;
    [[nodiscard]] virtual bool asBoolean()const noexcept         =0;
    [[nodiscard]] virtual Date asDate()const noexcept            =0;
    [[nodiscard]] virtual Time asTime()const noexcept            =0;
    [[nodiscard]] virtual DateTime asDateTime()const noexcept    =0;
    [[nodiscard]] virtual std::vector<Value::ptr> asArray() const noexcept   =0;
    [[nodiscard]] virtual std::unordered_map<std::string,Value::ptr> asTable() const noexcept   =0;

};


class Node
{
 public:
	explicit Node(toml::node_view<toml::node> node)
		:m_node(node){}
	~Node()=default;

	toml::node_view<toml::node> node() const noexcept
	{
		return m_node;
	}
 private:
	toml::node_view<toml::node> m_node;
};

class Value4Toml:public Value
{
public:
    using ptr=std::shared_ptr<Value>;
    using unique_ptr=std::unique_ptr<Value>;

	explicit Value4Toml(Node&& node);

    ~Value4Toml() override=default;

    bool isString()const noexcept override
	{
		return m_node.node().is_string();
	}
    bool isInteger()const noexcept override
	{
		return m_node.node().is_integer();
	}
    bool isFloat()const noexcept override
	{
		return m_node.node().is_floating_point();
	}
    bool isBoolean()const noexcept override
	{
		return m_node.node().is_boolean();
	}
    bool isDate()const noexcept override
	{
		return m_node.node().is_date();
	}
    bool isTime()const noexcept override
	{
		return m_node.node().is_time();
	}
    bool isDateTime()const noexcept override
	{
		return m_node.node().is_date_time();
	}
    bool isArray() const noexcept override
	{
		return m_node.node().is_array();
	}
    bool isTable() const noexcept override
	{
		return m_node.node().is_table();
	}

    [[nodiscard]] std::string asString()const noexcept override
	{
		return m_node.node().as_string()->get();
	}
    [[nodiscard]] std::int64_t asInteger()const noexcept override
	{
		return m_node.node().as_integer()->get();
	}
    [[nodiscard]] double asFloat()const noexcept override
	{
		return m_node.node().as_floating_point()->get();
	}
    [[nodiscard]] bool asBoolean()const noexcept override
	{
		return m_node.node().as_boolean()->get();
	}
    [[nodiscard]] Date asDate()const noexcept override
	{
		return Date();
	}
    [[nodiscard]] Time asTime()const noexcept override
	{
		return Time();
	}
    [[nodiscard]] DateTime asDateTime()const noexcept override
	{
		return DateTime();
	}
    [[nodiscard]] std::vector<Value::ptr> asArray() const noexcept override
	{
		return std::vector<Value::ptr>();
	}
    [[nodiscard]] std::unordered_map<std::string,Value::ptr> asTable() const noexcept override
	{
		return  std::unordered_map<std::string,Value::ptr>();
	}

private:
	Node m_node;
};

template<typename T>
class Value_view
{
public:
    using Value_type=T;
    explicit Value_view(std::reference_wrapper<Value_type> value):m_value(value) {}

    ~Value_view()=default;

    bool isString()const noexcept{return m_value.isString();}
    bool isInteger()const noexcept{return m_value.isInteger();}
    bool isFloat()const noexcept{return m_value.isFloat();}
    bool isBoolean()const noexcept{return m_value.isBoolean();};
    bool isDate()const noexcept{return m_value.isDate();};
    bool isTime()const noexcept{return m_value.isTime();}
    bool isDateTime()const noexcept{return m_value.isDateTime();}
    bool isArray() const noexcept{return m_value.isArray();}
    bool isTable() const noexcept{return m_value.isTable();}


    [[nodiscard]] std::string asString()const noexcept{return m_value.get().asString();}
    [[nodiscard]] std::int64_t asInteger()const noexcept{return m_value.get().asInteger();}
    [[nodiscard]] double asFloat()const noexcept{return m_value.get().asFloat();}
    [[nodiscard]] bool asBoolean()const noexcept{return m_value.get().asBoolean();}
    [[nodiscard]] Date asDate()const noexcept{return m_value.get().asDate();}
    [[nodiscard]] Time asTime()const noexcept{return m_value.get().asTime();}
    [[nodiscard]] DateTime asDateTime()const noexcept{return m_value.get().asDateTime();}
    [[nodiscard]] std::vector<Value::ptr> asArray() const noexcept{return m_value.get().asArray();}
    [[nodiscard]] std::unordered_map<std::string,Value::ptr> asTable() const noexcept{return m_value.get().asTable();}

private:
    std::reference_wrapper<Value_type> m_value;
};


class ConfigBase:noncopyable
{
public:
    ConfigBase(std::string_view ConfigName,std::string_view Description)
    :m_configName(ConfigName),m_description(Description){}

    virtual ~ConfigBase()=default;

    virtual void LoadFile(std::string_view file)=0;
    virtual void Load(std::string_view doc)=0;
    virtual Value_view<Value> getConfig(std::string_view configName) =0 ;
    virtual Value_view<Value> getConfig(std::string_view configName,Value::ptr defaultValue) noexcept=0 ;

protected:
    std::string m_configName;
    std::string m_description;

};


class Handle;
class ConfigForToml:public ConfigBase
{
public:
    using ptr=std::shared_ptr<ConfigForToml>;
	using Values=std::unordered_map<std::string,Value::uniqe_ptr>;

    ConfigForToml(std::string_view ConfigName,std::string_view Description);

    ~ConfigForToml() override;

    void LoadFile(std::string_view file) override;
    void Load(std::string_view doc) override;

    Value_view<Value> getConfig(std::string_view configPath) override ;
    Value_view<Value> getConfig(std::string_view configPath,Value::ptr defaultValue) noexcept override;

private:
	std::unique_ptr<Handle> m_handle;
	Values m_values;
	/// @brief 是否载入配置
	bool m_isLoaded;
	MutexLock m_mutex;
};