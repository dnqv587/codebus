#include "Value.h"

NullValue NullValue::NullValue_v;

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
        array_t vec;
        for(auto& n : *val)
        {
            Value::uniqe_ptr value = std::make_unique<Value4Toml>(Node (toml::node_view<toml::node>(n)));
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
            Value::uniqe_ptr value = std::make_unique<Value4Toml>(Node (toml::node_view<toml::node>(n.second)));
            table.insert(std::make_pair(std::string(n.first.str()), std::move(value)));
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
        std::vector<table_t> arrayTable;

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
