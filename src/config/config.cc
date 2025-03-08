#include <config/config.h>
#include <utility>
#include "exception/Exception.hpp"
#include "log/Logger.h"

namespace bus
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
		Value::uniqe_ptr val;
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
	decltype(m_configs)::iterator iter;
	{
		MutexLockGuard lock1(m_mutex);
		iter = m_configs.find(configName);
	}

	if(iter != m_configs.cend())
	{
		MutexLockGuard lock2(m_mutex);
		try
		{
			iter->second->LoadFile(configPath);
		}
		catch (...)
		{
			throw;
		}
	}
	else
	{
		auto ptr = std::make_unique<ConfigForToml>(configName);
		try
		{
			ptr->LoadFile(configPath);
		}
		catch (...)
		{
			throw;
		}
		{
			MutexLockGuard lock2(m_mutex);
			m_configs.insert_or_assign(configName, std::move(ptr));
		}

	}
}

ConfigObj* ConfigManager::getConfig(const std::string& configName)
{
	MutexLockGuard lock(m_mutex);
	auto config = m_configs.find(configName);
	if (config != m_configs.end())
	{
		return config->second.get();
	}
	//TODO:这里需要根据配置文件类型进行构造对应的对象
	auto [iter,flag] = m_configs.emplace(std::make_pair(configName,std::make_unique<ConfigForToml>(configName)));
	if(!flag)
	{
		throw Exception::ConfigError("ConfigManager::getConfig insert error!");
	}
	return iter->second.get();
}
}