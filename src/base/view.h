#pragma once
#include <functional>
#include "Macro.h"

namespace bus
{
//***********************************************
// class     : View
// brief     : 视图类，不拥有对象生存周期
// attention : None
//***********************************************
template<typename T>
class View
{
 public:
	constexpr explicit View(std::reference_wrapper<T> value)
		: m_value(std::move(value))
	{
	}

	~View() = default;

	/// @brief 获取对象引用
	ATTR_PURE_INLINE
	constexpr T& Peer() const noexcept
	{
		return m_value.get();
	}
	ATTR_PURE_INLINE
	constexpr T* NativeHandle() const noexcept
	{
		return &(m_value.get());
	}

 protected:
	std::reference_wrapper<T> m_value;
};

//template<typename T>
//View<T>:: ~View()=default;
}