#pragma once
#include <optional>
#include <memory>
#include <cstdint>
#include "base/Macro.h"
#include "base/View.hpp"
#include "time/DateTime.h"

class NullValue;
class Value;
class Table;
class Value_view:public View<Value>
{
public:
    using table_t=Table;
    using array_t=std::vector<std::unique_ptr<Value>>;

    explicit Value_view(std::reference_wrapper<Value> value) noexcept
            :View<Value>{value}
    {}

    explicit Value_view(std::nullptr_t value) noexcept;

    ~Value_view() override=default;
    ATTR_NODISCARD
    bool isString()const noexcept;
    ATTR_NODISCARD
    bool isInteger()const noexcept;
    ATTR_NODISCARD
    bool isFloat()const noexcept;
    ATTR_NODISCARD
    bool isBoolean()const noexcept;;
    ATTR_NODISCARD
    bool isDate()const noexcept;;
    ATTR_NODISCARD
    bool isTime()const noexcept;
    ATTR_NODISCARD
    bool isDateTime()const noexcept;
    ATTR_NODISCARD
    bool isArray() const noexcept;
    ATTR_NODISCARD
    bool isTable() const noexcept;


    ATTR_NODISCARD
    std::optional<std::string> asString()const noexcept;
    ATTR_NODISCARD
    std::optional<std::int64_t> asInteger()const noexcept;
    ATTR_NODISCARD
    std::optional<double> asFloat()const noexcept;
    ATTR_NODISCARD
    std::optional<bool> asBoolean()const noexcept;
    ATTR_NODISCARD
    std::optional<Date> asDate()const noexcept;
    ATTR_NODISCARD
    std::optional<Time> asTime()const noexcept;
    ATTR_NODISCARD
    std::optional<DateTime> asDateTime()const noexcept;
    ATTR_NODISCARD
    std::optional<array_t> asArray() const noexcept;
    ATTR_NODISCARD
    std::optional<table_t> asTable() const noexcept;
    ATTR_NODISCARD
    std::optional<std::vector<table_t>> asArrayTable() const noexcept ;

private:
    using View::m_value;
};