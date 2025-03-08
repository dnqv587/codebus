#include "Value_view.h"
#include "config/Value.h"

namespace bus
{
bool Value_view::isString() const noexcept
{
	return m_value.get().isString();
}

bool Value_view::isInteger() const noexcept
{
	return m_value.get().isInteger();
}

bool Value_view::isFloat() const noexcept
{
	return m_value.get().isFloat();
}

bool Value_view::isBoolean() const noexcept
{
	return m_value.get().isBoolean();
}

bool Value_view::isDate() const noexcept
{
	return m_value.get().isDate();
}

bool Value_view::isTime() const noexcept
{
	return m_value.get().isTime();
}

bool Value_view::isDateTime() const noexcept
{
	return m_value.get().isDateTime();
}

bool Value_view::isArray() const noexcept
{
	return m_value.get().isArray();
}

bool Value_view::isTable() const noexcept
{
	return m_value.get().isTable();
}

std::optional<double> Value_view::asFloat() const noexcept
{
	return m_value.get().asFloat();
}

std::optional<bool> Value_view::asBoolean() const noexcept
{
	return m_value.get().asBoolean();
}

std::optional<Date> Value_view::asDate() const noexcept
{
	return m_value.get().asDate();
}

std::optional<Time> Value_view::asTime() const noexcept
{
	return m_value.get().asTime();
}

std::optional<DateTime> Value_view::asDateTime() const noexcept
{
	return m_value.get().asDateTime();
}

std::optional<Value_view::array_t> Value_view::asArray() const noexcept
{
	return m_value.get().asArray();
}

std::optional<Value_view::table_t> Value_view::asTable() const noexcept
{
	return m_value.get().asTable();
}

std::optional<std::vector<Value_view::table_t>> Value_view::asArrayTable() const noexcept
{
	return m_value.get().asArrayTable();
}

std::optional<std::string> Value_view::asString() const noexcept
{
	return m_value.get().asString();
}

std::optional<std::int64_t> Value_view::asInteger() const noexcept
{
	return m_value.get().asInteger();
}

Value_view::Value_view(std::nullptr_t value) noexcept
	: m_value{ std::ref(*(&NullValue::NullValue_v)) }
{
}

}