#include "Config.h"
#include "exception/Exception.hpp"
#include "base/Macro.h"
#include <toml/tomlc17.h>
#include <any>
#include <functional>
#include <mutex>


namespace bus {

constexpr const char* DEFAULT_PATH = "./sys.toml";

class Node{
public:
  using ref = std::reference_wrapper<const Node>;

  explicit Node(const toml_datum_t&& datum) : m_node(std::forward<const toml_datum_t>(datum)) {}

  ~Node() = default;

  toml_type_t type() const noexcept { return m_node.type; }

  uint32_t flag() const noexcept { return m_node.flag; }

  std::any data() const noexcept {
      return m_node.u;
  }

  const toml_datum_t* getHandle() const noexcept{
    return &m_node;
  }
private:
  const toml_datum_t m_node;
};

class Handle : noncopyable {
 public:
  using ptr = std::shared_ptr<Handle>;

  explicit Handle(std::string_view input, bool isFile = true) noexcept
  :m_result(isFile?toml_parse_file_ex(input.data()):toml_parse(input.data(),input.length()))
    ,m_top(std::move(m_result.toptab))
   {}
  
  ~Handle(){
    toml_free(m_result);
  }

  ATTR_PURE_INLINE
  bool ok() const noexcept{
    return m_result.ok;
  }

  ATTR_PURE_INLINE
  const char* errmsg() const noexcept{
    return m_result.errmsg;
  }

  ATTR_NODISCARD
  const Node::ref toptab() const noexcept {
    return std::ref(m_top);
  }

 private:
  const toml_result_t m_result;
  const Node m_top;
};


class Value4Toml : public Value
{
 public:
   using ptr = std::shared_ptr<Value4Toml>;
    /// @brief Value4Toml 需要访问 toml::Datum 的原始数据和存储
    /// @param datum 指向 toml::Datum 的指针，必须保证 datum 的生命周期长于 Value4Toml
    /// @param config Config4Toml 对象，用于访问 datum 存储
   //  Value4Toml(void* datum, Config4Toml& config) noexcept;
    Value4Toml(std::shared_ptr<Handle> handle,std::unique_ptr<Node>&& node) noexcept;
    ~Value4Toml() noexcept override = default;

    // 实现 value 的所有纯虚函数
    bool isString() const noexcept final;
    bool isInteger() const noexcept final;
    bool isFloat() const noexcept final;
    bool isBoolean() const noexcept final;
    bool isDate() const noexcept final;
    bool isTime() const noexcept final;
    bool isDateTime() const noexcept final;
    bool isArray() const noexcept final;
    bool isTable() const noexcept final;

    std::optional<std::string_view> asString() const noexcept final;
    std::optional<std::int64_t> asInteger() const noexcept final;
    std::optional<double> asFloat() const noexcept final;
    std::optional<bool> asBoolean() const noexcept final;
    std::optional<Date> asDate() const noexcept final;
    std::optional<Time> asTime() const noexcept final;
    std::optional<DateTime> asDateTime() const noexcept final;
    std::optional<array_t> asArray() const noexcept final;
    std::optional<table_t> asTable() const noexcept final;
    std::optional<std::vector<table_t>> asArrayTable() const noexcept final;
    std::optional<Value::ptr> seek(std::string_view config_name) const noexcept final;

 private:
    ATTR_PURE_INLINE
    std::ostream& print_value(std::ostream& os) const noexcept final;

    std::shared_ptr<Handle> m_handle;
    std::unique_ptr<Node> m_node;
};

/// @brief 空值单例，用于表示键不存在或类型不匹配
class NullValue : public Value
{
 public:
    NullValue() noexcept = default;
    ~NullValue() noexcept override = default;

    // 所有 is*() 方法返回 false
    ATTR_PURE_INLINE
    bool isString() const noexcept override { return false; }
    ATTR_PURE_INLINE
    bool isInteger() const noexcept override { return false; }
    ATTR_PURE_INLINE
    bool isFloat() const noexcept override { return false; }
    ATTR_PURE_INLINE
    bool isBoolean() const noexcept override { return false; }
    ATTR_PURE_INLINE
    bool isDate() const noexcept override { return false; }
    ATTR_PURE_INLINE
    bool isTime() const noexcept override { return false; }
    ATTR_PURE_INLINE
    bool isDateTime() const noexcept override { return false; }
    ATTR_PURE_INLINE
    bool isArray() const noexcept override { return false; }
    ATTR_PURE_INLINE
    bool isTable() const noexcept override { return false; }

    // 所有 as*() 方法返回 std::nullopt
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<std::string_view> asString() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<std::int64_t> asInteger() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<double> asFloat() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<bool> asBoolean() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<Date> asDate() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<Time> asTime() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<DateTime> asDateTime() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<array_t> asArray() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<table_t> asTable() const noexcept override { return std::nullopt; }
    ATTR_NODISCARD
    ATTR_INLINE
    std::optional<std::vector<table_t>> asArrayTable() const noexcept override { return std::nullopt; }

    std::optional<Value::ptr> seek(std::string_view config_name) const noexcept override {return std::nullopt;};

    /// @brief 获取 NullValue 单例
    ATTR_NODISCARD
    static Value::ptr getInstance() noexcept
    {
        static Value::ptr instance = std::make_shared<NullValue>();
        return instance;
    }

 private:
    ATTR_INLINE
    std::ostream& print_value(std::ostream& os) const noexcept override
    {
        os << "NullValue";
        return os;
    }
};

/*************************************************************************************************************************************************/

struct SYSCONF_Initialize{
    SYSCONF_Initialize(){
        std::filesystem::path default_path {DEFAULT_PATH};
        std::error_code ec;
        if(std::filesystem::exists(default_path,ec)){
            sys_config.LoadFile(DEFAULT_PATH);
        }
    }
};

SYSCONF_Initialize init_config;

void Config::LoadFile(std::filesystem::path file)
{
    std::unique_lock lock(m_mutex);
    try {
        m_handle = std::make_shared<Handle>(file.c_str(), true);

        if (!m_handle->ok()) {
            throw Exception::ConfigError(m_handle->errmsg());
        }

        m_isLoaded = true;
    } catch (...) {
        m_isLoaded = false;
        throw;
    }
}

void Config::Load(std::string_view doc)
{
    std::unique_lock lock(m_mutex);

    try {
        m_handle = std::make_shared<Handle>(std::string(doc), false);

        if (!m_handle->ok()) {
            throw Exception::ConfigError(m_handle->errmsg());
        }

        m_isLoaded = true;

    } catch (...) {
        m_isLoaded = false;
        throw;
    }
}

Value::ptr Config::get(std::string_view configName)
{
    std::shared_lock lock(m_mutex);

    if (UNLIKELY(!m_isLoaded)) {
        throw Exception::ConfigUnLoad();
    }

    Node top = m_handle->toptab();

    auto ret = toml_seek(*top.getHandle(), configName.data());
    if (!ret.type) {
      return NullValue::getInstance();
    }

    return std::make_shared<Value4Toml>(m_handle,std::make_unique<Node>(std::move(ret)));
}

Value4Toml::Value4Toml(std::shared_ptr<Handle> handle,std::unique_ptr<Node>&& node) noexcept
    : m_handle(handle),
    m_node(std::forward<std::unique_ptr<Node>>(node))
{
}

bool Value4Toml::isString() const noexcept
{
    return m_node->type() == TOML_STRING;
}

bool Value4Toml::isInteger() const noexcept
{
    return m_node->type() == TOML_INT64;
}

bool Value4Toml::isFloat() const noexcept
{
    return m_node->type() == TOML_FP64;
}

bool Value4Toml::isBoolean() const noexcept
{
    return m_node->type() == TOML_BOOLEAN;
}

bool Value4Toml::isDate() const noexcept
{
    return m_node->type() == TOML_DATE;
}

bool Value4Toml::isTime() const noexcept
{
    return m_node->type() == TOML_TIME;
}

bool Value4Toml::isDateTime() const noexcept
{
    return m_node->type() == TOML_DATETIME || m_node->type() == TOML_DATETIMETZ;
}

bool Value4Toml::isArray() const noexcept
{
    return m_node->type() == TOML_ARRAY;
}

bool Value4Toml::isTable() const noexcept
{
    return m_node->type() == TOML_TABLE;
}

std::optional<std::string_view> Value4Toml::asString() const noexcept
{
    if (!isString()) {
        return std::nullopt;
    }
    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        return std::make_optional<std::string_view>(node.str.ptr, (size_t)node.str.len);
    }
    return std::nullopt;
}

std::optional<std::int64_t> Value4Toml::asInteger() const noexcept
{
    if (!isInteger()) {
        return std::nullopt;
    }
    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        return std::make_optional<std::int64_t>(node.int64);
    }
    return std::nullopt;
}

std::optional<double> Value4Toml::asFloat() const noexcept
{
    if (!isFloat()) {
        return std::nullopt;
    }
    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        return std::make_optional<double>(node.fp64);
    }
    return std::nullopt;
}

std::optional<bool> Value4Toml::asBoolean() const noexcept
{
    if (!isBoolean()) {
        return std::nullopt;
    }
    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        return std::make_optional<bool>(!!node.boolean);
    }
    return std::nullopt;
}

std::optional<Value::Date> Value4Toml::asDate() const noexcept
{
    using namespace std::chrono;
    if (!isDate()) {
        return std::nullopt;
    }
    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        return std::make_optional<Value::Date>(year(node.ts.year), month(node.ts.month), day(node.ts.day));
    }
    return std::nullopt;
}

std::optional<Value::Time> Value4Toml::asTime() const noexcept
{
    using namespace std::chrono;
    if (!isTime()) {
        return std::nullopt;
    }
    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        return std::make_optional<Value::Time>(hours(node.ts.hour) + minutes(node.ts.minute) +
                  seconds(node.ts.second) + microseconds(node.ts.usec));
    }
    return std::nullopt;
}

std::optional<Value::DateTime> Value4Toml::asDateTime() const noexcept
{
    using namespace std::chrono;
    if (!isDateTime()) {
        return std::nullopt;
    }
    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        year_month_day ymd {year(node.ts.year), month(node.ts.month), day(node.ts.day)};
        auto tod = hours{node.ts.hour} + minutes{node.ts.minute} + seconds{node.ts.second} + microseconds{node.ts.usec};
        return std::make_optional<Value::DateTime>(sys_days(ymd) + tod);
    }
    return std::nullopt;
}

std::optional<Value::array_t> Value4Toml::asArray() const noexcept
{
    if (!isArray()) {
        return std::nullopt;
    }

    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        Value::array_t arr;
        arr.reserve(node.arr.size);
        for(int i=0;i<node.arr.size;++i){
            Value4Toml::ptr v = std::make_shared<Value4Toml>(m_handle,std::make_unique<Node>(std::move(*(node.arr.elem+i))));
            arr.push_back(std::move(v));
        }
        return {std::move(arr)};
    }
    return std::nullopt;
}

std::optional<Value::table_t> Value4Toml::asTable() const noexcept
{
    if (!isTable()) {
        return std::nullopt;
    }
    auto datum =m_node->data();
    if (LIKELY(datum.has_value())) {
        auto node = std::any_cast<toml_datum_t::data>(datum);
        Value::table_t table;
        table.reserve(node.tab.size);
        for(int i=0;i<node.tab.size;++i){
            std::string_view key {node.tab.key[i],static_cast<std::string_view::size_type>(node.tab.len[i])};
            Value::ptr v = std::make_shared<Value4Toml>(m_handle,std::make_unique<Node>(std::move(node.tab.value[i])));

            table.emplace(key,v);
        }
        return {std::move(table)};
    }
    return std::nullopt;
}

std::optional<std::vector<Value::table_t>> Value4Toml::asArrayTable() const noexcept
{
    return std::nullopt;
    // std::vector<value::table_t> result;
    // const array_t& arr = arrOpt.value();

    // for (const auto& elem : arr) {
    //     auto tableOpt = elem->asTable();
    //     if (tableOpt.has_value()) {
    //         result.emplace_back(tableOpt.value());
    //     }
    // }

    // return result;
}


std::optional<Value::ptr> Value4Toml::seek(std::string_view config_name) const noexcept{
    auto ret = toml_seek(*m_node->getHandle(), config_name.data());
    if (!ret.type) {
      return NullValue::getInstance();
    }

    return std::make_shared<Value4Toml>(m_handle,std::make_unique<Node>(std::move(ret)));
}
//***********************************************
std::ostream& Value4Toml::print_value(std::ostream& os) const noexcept
{

    auto datum = std::any_cast<toml_datum_t::data>(m_node->data());

    switch (m_node->type()) {
        case TOML_STRING:
            os << datum.str.ptr;
            break;
        case TOML_INT64:
            os << datum.int64;
            break;
        case TOML_FP64:
            os << datum.fp64;
            break;
        case TOML_BOOLEAN:
            os << (datum.boolean ? "true" : "false");
            break;
        case TOML_ARRAY:
            os << "[array]";
            break;
        case TOML_TABLE:
            os << "[table]";
            break;
        case TOML_DATE:
            os << "[date]";
            break;
        case TOML_TIME:
            os << "[time]";
            break;
        case TOML_DATETIME:
        case TOML_DATETIMETZ:
            os << "[datetime]";
            break;
        default:
            os << "[unknown]";
            break;
    }

    return os;
}

} // namespace bus
