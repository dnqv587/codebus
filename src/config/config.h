//***********************************************
// name      : config.h
// brief     : 配置文件
// attention : None
//***********************************************
#pragma once
#include <base/noncopyable.hpp>
#include <base/copyable.hpp>
#include <thread/Mutex.hpp>
#include <time/DateTime.h>
#include <toml++/toml.hpp>
#include <base/View.hpp>
#include <string>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
#include <iostream>


class Value_view;
class Value:copyable
{
 public:
    using ptr=std::shared_ptr<Value>;
	using uniqe_ptr=std::unique_ptr<Value>;
	using weak_ptr=std::weak_ptr<Value>;

    using table_t=std::unordered_map<std::string,Value::ptr>;
    using array_t=std::vector<Value::ptr>;

    Value()=default;
    virtual ~Value()=default;
    ATTR_PURE_INLINE
    virtual bool isString()const noexcept   =0;
    ATTR_PURE_INLINE
    virtual bool isInteger()const noexcept  =0;
    ATTR_PURE_INLINE
    virtual bool isFloat()const noexcept    =0;
    ATTR_PURE_INLINE
    virtual bool isBoolean()const noexcept  =0;
    ATTR_PURE_INLINE
    virtual bool isDate()const noexcept     =0;
    ATTR_PURE_INLINE
    virtual bool isTime()const noexcept     =0;
    ATTR_PURE_INLINE
    virtual bool isDateTime()const noexcept =0;
    ATTR_PURE_INLINE
    virtual bool isArray() const noexcept   =0;
    ATTR_PURE_INLINE
    virtual bool isTable() const noexcept   =0;

    ATTR_NODISCARD
    virtual std::optional<std::string> asString() const noexcept               =0;
    ATTR_NODISCARD
    virtual std::optional<std::int64_t> asInteger() const noexcept             =0;
    ATTR_NODISCARD
    virtual std::optional<double> asFloat()const  noexcept                     =0;
    ATTR_NODISCARD
    virtual std::optional<bool> asBoolean()const noexcept                      =0;
    ATTR_NODISCARD
    virtual std::optional<Date> asDate()const noexcept                         =0;
    ATTR_NODISCARD
    virtual std::optional<Time> asTime()const noexcept                         =0;
    ATTR_NODISCARD
    virtual std::optional<DateTime> asDateTime()const noexcept                 =0;
    ATTR_NODISCARD
    virtual std::optional<array_t> asArray() const noexcept                    =0;
    ATTR_NODISCARD
    virtual  std::optional<table_t> asTable() const noexcept                   =0;
    ATTR_NODISCARD
	virtual std::optional<std::vector<table_t>> asArrayTable() const noexcept  =0;

protected:
    friend std::ostream& operator<<(std::ostream& lhs,const Value& rhs);
    virtual std::ostream& print_value(std::ostream& os) const noexcept          =0;

};




class Value_view:public View<Value>
{
 public:
    using table_t=std::unordered_map<std::string,Value::ptr>;
    using array_t=std::vector<Value::ptr>;

	explicit Value_view(std::reference_wrapper<Value> value)
        :View<Value>{value}
    {}


    ~Value_view() override=default;
    ATTR_PURE_INLINE
	bool isString()const noexcept{return m_value.get().isString();}
    ATTR_PURE_INLINE
	bool isInteger()const noexcept{return m_value.get().isInteger();}
    ATTR_PURE_INLINE
	bool isFloat()const noexcept{return m_value.get().isFloat();}
    ATTR_PURE_INLINE
	bool isBoolean()const noexcept{return m_value.get().isBoolean();};
    ATTR_PURE_INLINE
	bool isDate()const noexcept{return m_value.get().isDate();};
    ATTR_PURE_INLINE
	bool isTime()const noexcept{return m_value.get().isTime();}
    ATTR_PURE_INLINE
	bool isDateTime()const noexcept{return m_value.get().isDateTime();}
    ATTR_PURE_INLINE
	bool isArray() const noexcept{return m_value.get().isArray();}
    ATTR_PURE_INLINE
	bool isTable() const noexcept{return m_value.get().isTable();}


    ATTR_PURE_INLINE
    std::optional<std::string> asString()const noexcept{return m_value.get().asString();}
    ATTR_PURE_INLINE
    std::optional<std::int64_t> asInteger()const noexcept{return m_value.get().asInteger();}
    ATTR_PURE_INLINE
    std::optional<double> asFloat()const noexcept{return m_value.get().asFloat();}
    ATTR_PURE_INLINE
    std::optional<bool> asBoolean()const noexcept{return m_value.get().asBoolean();}
    ATTR_PURE_INLINE
    std::optional<Date> asDate()const noexcept{return m_value.get().asDate();}
    ATTR_PURE_INLINE
    std::optional<Time> asTime()const noexcept{return m_value.get().asTime();}
    ATTR_PURE_INLINE
    std::optional<DateTime> asDateTime()const noexcept{return m_value.get().asDateTime();}
    ATTR_PURE_INLINE
    std::optional<array_t> asArray() const noexcept{return m_value.get().asArray();}
    ATTR_PURE_INLINE
    std::optional<table_t> asTable() const noexcept{return m_value.get().asTable();}
    ATTR_PURE_INLINE
	std::optional<std::vector<table_t>> asArrayTable() const noexcept {return m_value.get().asArrayTable();}

 private:
    using View::m_value;
};

class ConfigBase:noncopyable
{
 public:
	using ref=std::reference_wrapper<ConfigBase>;
    using ptr=std::shared_ptr<ConfigBase>;
    using unique_ptr=std::unique_ptr<ConfigBase>;
	
	explicit ConfigBase(std::string  configName)
	:m_Name(std::move(configName)){}

	virtual ~ConfigBase()=default;

	virtual void LoadFile(std::string_view file)=0;
	virtual void Load(std::string_view doc)=0;
	virtual Value_view getConfig(std::string_view configName) =0 ;
 private:
	std::string m_Name;
};


class Node
{
 public:
	explicit Node(toml::node_view<toml::node> node)
		:m_node(node){}
	~Node()=default;

    ATTR_PURE_INLINE
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
    using Value::table_t;
    using Value::array_t;

	explicit Value4Toml(Node&& node);

    ~Value4Toml() override=default;

    bool isString()const noexcept override;

    bool isInteger()const noexcept override;

    bool isFloat()const noexcept override;

    bool isBoolean()const noexcept override;

    bool isDate()const noexcept override;

    bool isTime()const noexcept override;

    bool isDateTime()const noexcept override;

    bool isArray() const noexcept override;

    bool isTable() const noexcept override;

    std::optional<std::string> asString()const noexcept final;

    std::optional<std::int64_t> asInteger()const noexcept final;

    std::optional<double> asFloat()const noexcept final;

    std::optional<bool> asBoolean()const noexcept final;

    std::optional<Date> asDate()const noexcept final;

    std::optional<Time> asTime()const noexcept final;

    std::optional<DateTime> asDateTime()const noexcept final;

    std::optional<array_t> asArray() const noexcept final;

    std::optional<table_t> asTable() const noexcept final;

    std::optional<std::vector<table_t>> asArrayTable() const noexcept final;

    friend std::ostream& operator<<(std::ostream& lhs,const Value& rhs)
    {
        return rhs.print_value(lhs);
    }

private:
    std::ostream& print_value(std::ostream& os) const noexcept final
    {
        os<<m_node.node();
        return os;
    }

	Node m_node;
};


class Handle;
class ConfigForToml:public ConfigBase
{
public:
    using ptr=std::shared_ptr<ConfigForToml>;
	using Values=std::unordered_map<std::string,Value::uniqe_ptr>;

    explicit ConfigForToml(std::string ConfigName) noexcept;

    ~ConfigForToml() override;

    void LoadFile(std::string_view file) override;
    void Load(std::string_view doc) override;

    Value_view getConfig(std::string_view configPath) override ;

private:
	std::unique_ptr<Handle> m_handle;
	Values m_values;
	/// @brief 是否载入配置
	bool m_isLoaded;
	MutexLock m_mutex;
};

class ConfigManager:noncopyable
{
 public:
	using ConfigMap=std::unordered_map<std::string,ConfigBase::unique_ptr>;

	static ConfigManager* getInstance() noexcept
	{
		return Singleton<ConfigManager>::getInstance();
	}

	void Load(const std::string& configName,const std::string& configPath);

    std::optional<ConfigBase::ref> getConfig(const std::string& configName) noexcept;

 private:
	ConfigMap m_configs;
    MutexLock m_mutex;
};