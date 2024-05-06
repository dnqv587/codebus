#include <config/config.h>
#include <utility>
#include "exception/Exception.hpp"
#include "log/Logger.h"

namespace uf
{
static Logger& g_logger = GET_LOGGER("system");

class Handle : noncopyable
{
 public:
	explicit Handle(toml::table&& table)
		: m_handle(std::forward<toml::table>(table))
	{
	}

	~Handle() = default;

	toml::table& refHandle()
	{
		return m_handle;
	}
 private:
	toml::table m_handle;
};

ConfigForToml::ConfigForToml(std::string ConfigName) noexcept:
	ConfigObj(std::move(ConfigName)),
	m_handle(nullptr),
	m_isLoaded(false)
{
}

void ConfigForToml::LoadFile(std::string_view file)
{

	try
	{
		MutexLockGuard lock(m_mutex);
		m_handle = std::make_unique<Handle>(toml::parse_file(file));
		m_isLoaded = true;
	}
	catch (...)
	{
		throw;
	}
}

void ConfigForToml::Load(std::string_view doc)
{
	try
	{
		MutexLockGuard lock(m_mutex);
		m_handle = std::make_unique<Handle>(toml::parse(doc));
		m_isLoaded = true;
	}
	catch (...)
	{
		throw;
	}
}

Value_view ConfigForToml::getConfigValue(std::string_view configPath)
{
	MutexLockGuard lock(m_mutex);
	if (!m_isLoaded)
	{
		throw Exception::ConfigUnLoad();
	}
	auto key = m_values.find(configPath.data());
	if (key != m_values.end())
	{
		return Value_view(std::ref(*key->second));
	}
	else
	{
		Value::uniqe_ptr val = nullptr;
		try
		{
			Node node(m_handle->refHandle().at_path(configPath));
			val = std::make_unique<Value4Toml>(std::move(node));
		}
		catch (...)
		{
			throw;
		}
		Value_view value(std::ref(*val));
		m_values.insert(std::make_pair(configPath, std::move(val)));
		return value;
	}
}

ConfigForToml::~ConfigForToml() = default;

Value4Toml::Value4Toml(Node&& node)
	: m_node(std::forward<Node>(node))
{
}

void ConfigManager::Load(const std::string& configName, const std::string& configPath)
{
	MutexLockGuard lock(m_mutex);
	auto ptr = std::make_unique<ConfigForToml>(configName);
	try
	{
		ptr->LoadFile(configPath);
	}
	catch (...)
	{
		throw;
	}
	m_configs.insert_or_assign(configName, std::move(ptr));
}

ConfigObj* ConfigManager::getConfig(const std::string& configName)
{
	MutexLockGuard lock(m_mutex);
	auto config = m_configs.find(configName);
	if (config == m_configs.end())
	{
		LOG_ERROR(g_logger) << "ConfigManager::getConfig error,no find config,config name = " << configName;
		throw Exception::ConfigError("non getting config,please check on ConfigName!");
	}
	return config->second.get();
}
}