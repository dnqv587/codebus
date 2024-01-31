#pragma once
#include <memory>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cstdint>
#include <toml++/toml.hpp>
#include "base/Macro.h"
#include "base/copyable.hpp"
#include "time/DateTime.h"
#include "Table.hpp"

class Value_view;

class Value:copyable
{
public:
    using ptr=std::shared_ptr<Value>;
    using uniqe_ptr=std::unique_ptr<Value>;
    using weak_ptr=std::weak_ptr<Value>;

    //using table_t=std::unordered_map<std::string,Value::uniqe_ptr>;
    using table_t=Table;
    using array_t=std::vector<Value::uniqe_ptr>;

    Value()=default;
    virtual ~Value()=default;
    ATTR_PURE_INLINE
    virtual bool isString()const noexcept   =0;
    ATTR_PURE_INLINE
    virtual bool isInteger()const noexcept  =0;
    ATTR_PURE_INLINE
    virtual bool isFloat()const noexcept    =0;
    ATTR_PURE_INLINE
    virtual bool isBoolean()const noexcept  =0;
    ATTR_PURE_INLINE
    virtual bool isDate()const noexcept     =0;
    ATTR_PURE_INLINE
    virtual bool isTime()const noexcept     =0;
    ATTR_PURE_INLINE
    virtual bool isDateTime()const noexcept =0;
    ATTR_PURE_INLINE
    virtual bool isArray() const noexcept   =0;
    ATTR_PURE_INLINE
    virtual bool isTable() const noexcept   =0;

    ATTR_NODISCARD
    virtual std::optional<std::string> asString() const noexcept               =0;
    ATTR_NODISCARD
    virtual std::optional<std::int64_t> asInteger() const noexcept             =0;
    ATTR_NODISCARD
    virtual std::optional<double> asFloat()const  noexcept                     =0;
    ATTR_NODISCARD
    virtual std::optional<bool> asBoolean()const noexcept                      =0;
    ATTR_NODISCARD
    virtual std::optional<Date> asDate()const noexcept                         =0;
    ATTR_NODISCARD
    virtual std::optional<Time> asTime()const noexcept                         =0;
    ATTR_NODISCARD
    virtual std::optional<DateTime> asDateTime()const noexcept                 =0;
    ATTR_NODISCARD
    virtual std::optional<array_t> asArray() const noexcept                    =0;
    ATTR_NODISCARD
    virtual  std::optional<table_t> asTable() const noexcept                   =0;
    ATTR_NODISCARD
    virtual std::optional<std::vector<table_t>> asArrayTable() const noexcept  =0;

    friend std::ostream& operator<<(std::ostream& lhs,const Value& rhs)
    {
        return rhs.print_value(lhs);
    }

protected:

    virtual std::ostream& print_value(std::ostream& os) const noexcept          =0;

};

class Node
{
public:
    explicit Node(toml::node_view<toml::node> node)
            :m_node(node){}
    ~Node()=default;

    ATTR_PURE_INLINE
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
    using Value::table_t;
    using Value::array_t;

    explicit Value4Toml(Node&& node);

    ~Value4Toml() override=default;

    bool isString()const noexcept final;

    bool isInteger()const noexcept final;

    bool isFloat()const noexcept final;

    bool isBoolean()const noexcept final;

    bool isDate()const noexcept final;

    bool isTime()const noexcept final;

    bool isDateTime()const noexcept final;

    bool isArray() const noexcept final;

    bool isTable() const noexcept final;

    std::optional<std::string> asString()const noexcept final;

    std::optional<std::int64_t> asInteger()const noexcept final;

    std::optional<double> asFloat()const noexcept final;

    std::optional<bool> asBoolean()const noexcept final;

    std::optional<Date> asDate()const noexcept final;

    std::optional<Time> asTime()const noexcept final;

    std::optional<DateTime> asDateTime()const noexcept final;

    std::optional<array_t> asArray() const noexcept final;

    std::optional<table_t> asTable() const noexcept final;

    std::optional<std::vector<table_t>> asArrayTable() const noexcept final;

private:
    ATTR_PURE_INLINE
    std::ostream& print_value(std::ostream& os) const noexcept final
    {
        os<<m_node.node();
        return os;
    }

    Node m_node;
};

class NullValue:public Value
{
public:
    NullValue()=default;
    ~NullValue() override =default;

    ATTR_PURE_INLINE
    bool isString()const noexcept override
    {
        return false;
    }
    ATTR_PURE_INLINE
    bool isInteger()const noexcept override
    {
        return false;
    }
    ATTR_PURE_INLINE
    bool isFloat()const noexcept final
    {
        return false;
    }
    ATTR_PURE_INLINE
    bool isBoolean()const noexcept final
    {
        return false;
    }
    ATTR_PURE_INLINE
    bool isDate()const noexcept final
    {
        return false;
    }
    ATTR_PURE_INLINE
    bool isTime()const noexcept final
    {
        return false;
    }
    ATTR_PURE_INLINE
    bool isDateTime()const noexcept final
    {
        return false;
    }
    ATTR_PURE_INLINE
    bool isArray() const noexcept final
    {
        return false;
    }
    ATTR_PURE_INLINE
    bool isTable() const noexcept final
    {
        return false;
    }

    ATTR_PURE_INLINE
    std::optional<std::string> asString()const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<std::int64_t> asInteger()const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<double> asFloat()const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<bool> asBoolean()const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<Date> asDate()const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<Time> asTime()const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<DateTime> asDateTime()const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<array_t> asArray() const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<table_t> asTable() const noexcept final
    {
        return std::nullopt;
    }
    ATTR_PURE_INLINE
    std::optional<std::vector<table_t>> asArrayTable() const noexcept final
    {
        return std::nullopt;
    }

    static  NullValue NullValue_v;

private:
    std::ostream& print_value(std::ostream& os) const noexcept final
    {
        os<<"NullValue";
        return os;
    }

};
