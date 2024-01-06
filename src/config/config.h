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

    [[nodiscard]]
    virtual std::string asString(std::string_view default_val) const noexcept   =0;
    [[nodiscard]]
    virtual std::string asString() const noexcept                               =0;
    [[nodiscard]]
    virtual std::int64_t asInteger(std::int64_t default_val) const noexcept     =0;
    [[nodiscard]]
    virtual std::int64_t asInteger() const noexcept                             =0;
    [[nodiscard]]
    virtual double asFloat(double default_val)const noexcept                    =0;
    [[nodiscard]]
    virtual double asFloat()const  noexcept                                     =0;
    [[nodiscard]]
    virtual bool asBoolean(bool default_val)const noexcept                      =0;
    [[nodiscard]]
    virtual bool asBoolean()const noexcept                                      =0;
    [[nodiscard]]
    virtual Date asDate(const Date& default_val)const noexcept                  =0;
    [[nodiscard]]
    virtual Date asDate()const noexcept                                         =0;
    [[nodiscard]]
    virtual Time asTime(const Time& default_val)const noexcept                  =0;
    [[nodiscard]]
    virtual Time asTime()const noexcept                                         =0;
    [[nodiscard]]
    virtual DateTime asDateTime(const DateTime& default_val)const noexcept      =0;
    [[nodiscard]]
    virtual DateTime asDateTime()const noexcept                                 =0;
    [[nodiscard]]
    virtual std::vector<Value::ptr> asArray() const noexcept                    =0;
    [[nodiscard]]
    virtual std::unordered_map<std::string,Value::ptr> asTable() const noexcept =0;
	[[nodiscard]]
	virtual std::vector<std::unordered_map<std::string,Value::ptr>> asArrayTable() const noexcept =0;

protected:
    friend std::ostream& operator<<(std::ostream& lhs,const Value& rhs);
    virtual std::ostream& print_value(std::ostream& os) const noexcept          =0;

};




class Value_view:public View<Value>
{
 public:

	explicit Value_view(std::reference_wrapper<Value> value)
        :View<Value>{value}
    {}


    ~Value_view() override=default;

	bool isString()const noexcept{return m_value.get().isString();}
	bool isInteger()const noexcept{return m_value.get().isInteger();}
	bool isFloat()const noexcept{return m_value.get().isFloat();}
	bool isBoolean()const noexcept{return m_value.get().isBoolean();};
	bool isDate()const noexcept{return m_value.get().isDate();};
	bool isTime()const noexcept{return m_value.get().isTime();}
	bool isDateTime()const noexcept{return m_value.get().isDateTime();}
	bool isArray() const noexcept{return m_value.get().isArray();}
	bool isTable() const noexcept{return m_value.get().isTable();}


	[[nodiscard]]
    std::string asString()const noexcept{return m_value.get().asString();}
    [[nodiscard]]
    std::string asString(std::string_view default_val)const noexcept{return m_value.get().asString(default_val);}
	[[nodiscard]]
    std::int64_t asInteger()const noexcept{return m_value.get().asInteger();}
    [[nodiscard]]
    std::int64_t asInteger(std::int64_t default_val)const noexcept{return m_value.get().asInteger(default_val);}
	[[nodiscard]]
    double asFloat()const noexcept{return m_value.get().asFloat();}
    [[nodiscard]]
    double asFloat(double default_val)const noexcept{return m_value.get().asFloat(default_val);}
	[[nodiscard]]
    bool asBoolean()const noexcept{return m_value.get().asBoolean();}
    [[nodiscard]]
    bool asBoolean(bool default_val)const noexcept{return m_value.get().asBoolean(default_val);}
	[[nodiscard]]
    Date asDate()const noexcept{return m_value.get().asDate();}
    [[nodiscard]]
    Date asDate(const Date& default_val)const noexcept{return m_value.get().asDate(default_val);}
	[[nodiscard]]
    Time asTime()const noexcept{return m_value.get().asTime();}
    [[nodiscard]]
    Time asTime(const Time& default_val)const noexcept{return m_value.get().asTime(default_val);}
	[[nodiscard]]
    DateTime asDateTime()const noexcept{return m_value.get().asDateTime();}
    [[nodiscard]]
    DateTime asDateTime(const DateTime& default_val)const noexcept{return m_value.get().asDateTime(default_val);}
    [[nodiscard]]
    std::vector<Value::ptr > asArray() const noexcept{return m_value.get().asArray();}
	[[nodiscard]]
    std::unordered_map<std::string,Value::ptr> asTable() const noexcept{return m_value.get().asTable();}
	[[nodiscard]]
	std::vector<std::unordered_map<std::string,Value::ptr>> asArrayTable() const noexcept {return m_value.get().asArrayTable();}

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

    bool isString()const noexcept override;

    bool isInteger()const noexcept override;

    bool isFloat()const noexcept override;

    bool isBoolean()const noexcept override;

    bool isDate()const noexcept override;

    bool isTime()const noexcept override;

    bool isDateTime()const noexcept override;

    bool isArray() const noexcept override;

    bool isTable() const noexcept override;

    [[nodiscard]]
    std::string asString()const noexcept override;
    [[nodiscard]]
    std::string asString(std::string_view default_val)const noexcept override;

    [[nodiscard]]
    std::int64_t asInteger()const noexcept override;
    [[nodiscard]]
    std::int64_t asInteger(std::int64_t default_val)const noexcept override;

    [[nodiscard]]
    double asFloat()const noexcept override;
    [[nodiscard]]
    double asFloat(double default_val)const noexcept override;

    [[nodiscard]]
    bool asBoolean()const noexcept override;
    [[nodiscard]]
    bool asBoolean(bool default_val)const noexcept override;

    [[nodiscard]]
    Date asDate()const noexcept override;
    [[nodiscard]]
    Date asDate(const Date& default_val) const noexcept override;

	[[nodiscard]]
    Time asTime()const noexcept override;
    [[nodiscard]]
    Time asTime(const Time& default_val)const noexcept override;

    [[nodiscard]]
    DateTime asDateTime()const noexcept override;
    [[nodiscard]]
    DateTime asDateTime(const DateTime& default_val)const noexcept override;

    [[nodiscard]]
    std::vector<Value::ptr> asArray() const noexcept override;

    [[nodiscard]]
    std::unordered_map<std::string,Value::ptr> asTable() const noexcept override;
	[[nodiscard]]
	std::vector<std::unordered_map<std::string,Value::ptr>> asArrayTable() const noexcept override;

    friend std::ostream& operator<<(std::ostream& lhs,const Value& rhs)
    {
        return rhs.print_value(lhs);
    }

private:
    std::ostream& print_value(std::ostream& os) const noexcept override
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