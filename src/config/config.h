//***********************************************
// name      : config.h
// brief     : 配置文件
// attention : None
//***********************************************
#pragma once
#include <base/noncopyable.hpp>
#include <base/copyable.hpp>
#include <thread/Mutex.hpp>
#include <string>
#include <memory>
#include "Value_view.h"
#include "Value.h"


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
    ATTR_NODISCARD
	virtual Value_view getConfig(std::string_view configName) =0 ;
 private:
	std::string m_Name;
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

    ATTR_NODISCARD
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

    ATTR_NODISCARD
    std::optional<ConfigBase::ref> getConfig(const std::string& configName) noexcept;

 private:
	ConfigMap m_configs;
    MutexLock m_mutex;
};