#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <optional>
#include "base/Macro.h"
#include "Value_view.h"

namespace uf
{
class Value;
using Value_ptr = std::unique_ptr<Value>;

template<bool IsConst>
struct table_pair
{
	using value_t = std::conditional_t<IsConst, const Value, Value>;

	std::string& first;
	value_t& second;
};

template<bool IsConst>
class table_iterator
{
	using map_t = typename std::unordered_map<std::string, Value_ptr>;
	using mutable_map_iterator = typename map_t::iterator;
	using const_map_iterator = typename map_t::const_iterator;
	using local_iterator = typename std::conditional_t<IsConst, const_map_iterator, mutable_map_iterator>;

 public:
	using value_t = table_pair<IsConst>;

	explicit table_iterator(mutable_map_iterator iter) noexcept
		: m_iter{ iter }
	{
	}

	template<bool C = IsConst, typename std::enable_if<C, int>::type = 0>
	explicit table_iterator(const_map_iterator iter) noexcept
		:m_iter{ iter }
	{
	}

	table_iterator(const table_iterator& other) noexcept
		: m_iter(other)
	{
	}

	table_iterator& operator=(const table_iterator& that)
	{
		if (this != &that)
		{
			m_iter = that.m_iter;
			m_pair.reset();
		}
		return *this;
	}

	value_t& operator*() const noexcept
	{
		return get_value();
	}

	value_t* operator->() noexcept
	{
		return &get_value();
	}

	table_iterator& operator++() noexcept
	{
		++m_iter;
		m_pair.reset();
		return *this;
	}

	table_iterator operator++(int) noexcept
	{
		table_iterator out{ m_iter };
		++m_iter;
		m_pair.reset();
		return out;
	}

	table_iterator& operator--() noexcept
	{
		--m_iter;
		m_pair.reset();
		return *this;
	}

	table_iterator operator--(int) noexcept
	{
		table_iterator out{ m_iter };
		--m_iter;
		m_pair.reset();
		return out;
	}

	explicit operator local_iterator&() const noexcept
	{
		return m_iter;
	}

	template<bool C = IsConst, typename std::enable_if<!C, int>::type = 0>
	explicit operator const const_map_iterator() const noexcept
	{
		return m_iter;
	}

	friend bool operator==(const table_iterator& lhs, const table_iterator& rhs) noexcept
	{
		return lhs.m_iter == rhs.m_iter;
	}

	friend bool operator!=(const table_iterator& lhs, const table_iterator& rhs) noexcept
	{
		return lhs.m_iter != rhs.m_iter;
	}

 private:
	ATTR_NODISCARD
	value_t& get_value() const noexcept
	{
		if (m_pair.has_value())
		{
			return *m_pair;
		}
		else
		{
			m_pair.emplace(value_t{ m_iter->first, m_iter->second });
			return *m_pair;
		}
	}

	std::optional<value_t> m_pair;
	local_iterator m_iter;
};

class Table
{
 public:
	/// type
	using key_t = typename std::string;
	using value_t = Value_ptr;
	using table_t = typename std::unordered_map<key_t, value_t>;
	using iterator = table_iterator<false>;
	using const_iterator = table_iterator<true>;
	using pair_t = typename table_t::value_type;
	using size_type = typename table_t::size_type;

	Table() = default;
	Table(const Table& other) = default;

	Table(Table&& other) noexcept
		: m_table(std::move(other.m_table))
	{
	}

	~Table() = default;

	Table& operator=(const Table& that) = default;

	Table& operator=(Table&& that) noexcept
	{
		if (&that != this)
		{
			this->m_table = std::move(that.m_table);
		}
		return *this;
	}
	ATTR_NODISCARD
	Value_view Lookup(const std::string& key) noexcept
	{
		if (auto iter = m_table.find(key);iter != m_table.end())
		{
			return Value_view(std::ref(*iter->second));
		}
		return Value_view(nullptr);
	}

	template<typename Pair>
	ATTR_INLINE
	std::pair<table_t::iterator, bool> insert(Pair&& val)
	{
		return m_table.emplace(std::forward<Pair>(val));
	}
	ATTR_INLINE
	std::pair<table_t::iterator, bool> insert(pair_t&& val)
	{
		return m_table.insert(std::move(val));
	}
	ATTR_PURE_INLINE
	iterator find(const key_t& key)
	{
		return iterator(m_table.find(key));
	}
	ATTR_PURE_INLINE
	const_iterator find(const key_t& key) const
	{
		return const_iterator(m_table.find(key));
	}
	ATTR_PURE_INLINE
	size_type count(const key_t& key) const
	{
		return m_table.count(key);
	}
	ATTR_PURE_INLINE
	size_type size() const noexcept
	{
		return m_table.size();
	}
	ATTR_PURE_INLINE
	iterator begin() noexcept
	{
		return iterator(m_table.begin());
	}
	ATTR_PURE_INLINE
	const_iterator begin() const noexcept
	{
		return const_iterator(m_table.begin());
	}
	ATTR_PURE_INLINE
	const_iterator cbegin() const noexcept
	{
		return const_iterator(m_table.begin());
	}
	ATTR_PURE_INLINE
	iterator end() noexcept
	{
		return iterator(m_table.end());
	}
	ATTR_PURE_INLINE
	const_iterator end() const noexcept
	{
		return const_iterator(m_table.end());
	}
	ATTR_PURE_INLINE
	const_iterator cend() const noexcept
	{
		return const_iterator(m_table.cend());
	}
	ATTR_INLINE
	void clear() noexcept
	{
		m_table.clear();
	}
	ATTR_PURE_INLINE
	bool empty() const noexcept
	{
		return m_table.empty();
	}

 private:
	table_t m_table;
};
}