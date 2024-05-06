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

namespace uf
{
class ConfigObj : noncopyable
{
 public:
	using ref = std::reference_wrapper<ConfigObj>;
	using ptr = std::shared_ptr<ConfigObj>;
	using unique_ptr = std::unique_ptr<ConfigObj>;

	explicit ConfigObj(std::string configName)
		: m_Name(std::move(configName))
	{
	}

	virtual ~ConfigObj() = default;

	virtual void LoadFile(std::string_view file) = 0;
	virtual void Load(std::string_view doc) = 0;
	ATTR_NODISCARD
	virtual Value_view getConfigValue(std::string_view configName) = 0;
 private:
	std::string m_Name;
};

class Handle;
class ConfigForToml : public ConfigObj
{
 public:
	using ptr = std::shared_ptr<ConfigForToml>;
	using Values = std::unordered_map<std::string, Value::uniqe_ptr>;

	explicit ConfigForToml(std::string ConfigName) noexcept;

	~ConfigForToml() override;

	void LoadFile(std::string_view file) override;
	void Load(std::string_view doc) override;

	ATTR_NODISCARD
	Value_view getConfigValue(std::string_view configPath) override;

 private:
	std::unique_ptr<Handle> m_handle;
	Values m_values;
	/// @brief 是否载入配置
	bool m_isLoaded;
	MutexLock m_mutex;
};

class ConfigManager : noncopyable
{
 public:
	using ConfigMap = std::unordered_map<std::string, ConfigObj::unique_ptr>;

	ConfigManager()
	{
		this->Load("root","test.toml");
	}

	static ConfigManager* getInstance() noexcept
	{
		return Singleton<ConfigManager>::getInstance();
	}
	/// @brief 解析配置文件
	/// @param configName 名称，root被系统默认占用
	/// @param configPath 路径
	void Load(const std::string& configName, const std::string& configPath);

	ATTR_NODISCARD
	ConfigObj* getConfig(const std::string& configName);

 private:
	ConfigMap m_configs;
	MutexLock m_mutex;
};

#define GET_CONFIG(cfg_name) ConfigManager::getInstance()->getConfig(cfg_name)
/// @brief 系统默认配置文件root
#define GET_ROOT_CONFIG(cfg_name) ConfigManager::getInstance()->getConfig("root")->getConfigValue(cfg_name)
}