#include <config/config.h>
#include <exception/Exception.hpp>

class Handle:noncopyable
{
 public:
	explicit Handle(toml::table&& table)
	:m_handle(std::forward<toml::table>(table)){}

	~Handle()=default;

	toml::table& refHandle()
	{
		return m_handle;
	}
 private:
	toml::table m_handle;
};



ConfigForToml::ConfigForToml(std::string_view ConfigName, std::string_view Description) :
ConfigBase(ConfigName,Description),
m_handle(nullptr),
m_isLoaded(false)
{
}

void ConfigForToml::LoadFile(std::string_view file)
{

    try {
		MutexLockGuard lock(m_mutex);
        m_handle=std::make_unique<Handle>(toml::parse_file(file));
		m_isLoaded=true;
    }
    catch (...){
        throw;
    }
}

void ConfigForToml::Load(std::string_view doc)
{
    try {
        MutexLockGuard lock(m_mutex);
        m_handle=std::make_unique<Handle>(toml::parse(doc));
        m_isLoaded=true;
    }
    catch (...){
        throw;
    }
}

Value_view<Value> ConfigForToml::getConfig(std::string_view configPath)
{
	MutexLockGuard lock(m_mutex);
	if(!m_isLoaded)
	{
        throw Exception::ConfigUnLoad();
	}
	auto key=m_values.find(configPath.data());
	if(key!=m_values.end())
	{
		return Value_view<Value> (std::ref(*key->second));
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
		Value_view<Value> value(std::ref(*val));
		m_values.insert(std::make_pair(configPath, std::move(val)));
		return value;
	}
}

Value_view<Value> ConfigForToml::getConfig(std::string_view configPath, Value::ptr defaultValue) noexcept
{
    MutexLockGuard lock(m_mutex);
    if(!m_isLoaded)
    {
        throw Exception::ConfigUnLoad();
    }

    Node node(m_handle->refHandle().at_path(configPath));
    Value4Toml toml(std::move(node));
    Value_view<Value> value(std::ref(toml));
    return value;
}

ConfigForToml::~ConfigForToml()=default;


Value4Toml::Value4Toml(Node&& node)
: m_node(std::forward<Node>(node))
{
}


