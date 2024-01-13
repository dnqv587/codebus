#include <config/config.h>
#include <exception/Exception.hpp>
#include <utility>


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

bool Value4Toml::isString()const noexcept
{
    return m_node.node().is_string();
}

bool Value4Toml::isInteger()const noexcept
{
    return m_node.node().is_integer();
}

bool Value4Toml::isFloat()const noexcept
{
    return m_node.node().is_floating_point();
}

bool Value4Toml::isBoolean()const noexcept
{
    return m_node.node().is_boolean();
}

bool Value4Toml::isDate()const noexcept
{
    return m_node.node().is_date();
}

bool Value4Toml::isTime()const noexcept
{
    return m_node.node().is_time();
}

bool Value4Toml::isDateTime()const noexcept
{
    return m_node.node().is_date_time();
}

bool Value4Toml::isArray() const noexcept
{
    return m_node.node().is_array();
}

bool Value4Toml::isTable() const noexcept
{
    return m_node.node().is_table();
}

std::optional<std::string> Value4Toml::asString() const noexcept
{
    auto val=m_node.node().as_string();
    return val?std::make_optional<std::string>(val->get()):std::nullopt;
}

std::optional<std::int64_t> Value4Toml::asInteger()const noexcept
{
    auto val=m_node.node().as_integer();
    return val?std::make_optional<std::int64_t>(val->get()):std::nullopt;
}

std::optional<double> Value4Toml::asFloat() const noexcept
{
    auto val=m_node.node().as_floating_point();
    return val?std::make_optional<double>(val->get()):std::nullopt;
}

std::optional<bool> Value4Toml::asBoolean()const noexcept
{
    auto val=m_node.node().as_boolean();
    return val?std::make_optional<bool>(val->get()):std::nullopt;
}

std::optional<Date> Value4Toml::asDate() const noexcept
{
    auto val=m_node.node().as_date();
    if(val)
    {
        auto& d=val->get();
        Date date(d.year,d.month,d.day);
        return date;
    }
    return std::nullopt;
}

std::optional<Time> Value4Toml::asTime() const noexcept
{
    auto val=m_node.node().as_time();
    if(val)
    {
        auto& t=val->get();
        Time time(t.hour,t.minute,t.second,t.nanosecond);
        return time;
    }
    return std::nullopt;
}

std::optional<Value4Toml::array_t> Value4Toml::asArray() const noexcept
{
    auto val=m_node.node().as_array();
    if(val)
    {
        std::vector<Value::ptr> vec;
        for(auto& n : *val)
        {
            Value::ptr value = std::make_shared<Value4Toml>(Node (toml::node_view<toml::node>(n)));
            vec.emplace_back(std::move(value));
        }
        return vec;
    }
    return std::nullopt;
}

std::optional<Value4Toml::table_t> Value4Toml::asTable() const noexcept
{
    auto val=m_node.node().as_table();
    if(val)
    {
		table_t table;
        for(auto& n:*val)
        {
            Value::ptr value = std::make_shared<Value4Toml>(Node (toml::node_view<toml::node>(n.second)));
            table.insert(std::make_pair(n.first,value));
        }
        return table;
    }
    return std::nullopt;
}

std::optional<DateTime> Value4Toml::asDateTime() const noexcept
{
    auto val=m_node.node().as_date_time();
    if(val)
    {
        toml::date_time& dt = val->get();

        DateTime datetime(Date(dt.date.year, dt.date.month, dt.date.day),
                          Time(dt.time.hour, dt.time.minute, dt.time.second, dt.time.nanosecond),TimeZoneOffset(dt.offset.has_value()?dt.offset->minutes:0));
        return datetime;
    }
    return std::nullopt;
}

std::optional<std::vector<Value4Toml::table_t>> Value4Toml::asArrayTable() const noexcept
{
    auto arr=this->asArray();
    if(arr.has_value())
    {
        std::vector<std::unordered_map<std::string, Value::ptr>> arrayTable;

        for(auto& i : arr.value())
        {
            auto table=i->asTable();
            if(table.has_value())
            {
                arrayTable.emplace_back(std::move(table).value());
            }
        }
        return arrayTable;
    }
    return std::nullopt;
}

ConfigForToml::ConfigForToml(std::string ConfigName) noexcept:
ConfigBase(std::move(ConfigName)),
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

Value_view ConfigForToml::getConfig(std::string_view configPath)
{
	MutexLockGuard lock(m_mutex);
	if(!m_isLoaded)
	{
        throw Exception::ConfigUnLoad();
	}
	auto key=m_values.find(configPath.data());
	if(key!=m_values.end())
	{
		return Value_view (std::ref(*key->second));
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


ConfigForToml::~ConfigForToml()=default;


Value4Toml::Value4Toml(Node&& node)
: m_node(std::forward<Node>(node))
{
}

void ConfigManager::Load(const std::string& configName, const std::string& configPath)
{
    MutexLockGuard lock(m_mutex);
    auto ptr=std::make_unique<ConfigForToml>(configName);
    try
    {
        ptr->LoadFile(configPath);
    }
    catch (...)
    {
        throw;
    }
    m_configs.insert_or_assign(configName,std::move(ptr));
}

std::optional<ConfigBase::ref> ConfigManager::getConfig(const std::string& configName) noexcept
{
    MutexLockGuard lock(m_mutex);
    auto config=m_configs.find(configName);
    if (config == m_configs.end())
    {
        //LOG_THROW(Exception::ConfigError("non getting config,please check on ConfigName!"));
		return std::nullopt;
    }
    return std::ref(*config->second);
}
