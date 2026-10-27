#pragma once
#include <functional>
#include "Macro.h"

namespace bus {
//***********************************************
// class     : View
// brief     : 视图类，不拥有对象生存周期
// attention : None
//***********************************************
template <typename T>
class View {
 public:
    /// @brief 从 std::reference_wrapper 构造（支持隐式转换）
    constexpr View(const std::reference_wrapper<T>& value) noexcept : m_value(value) {}

    constexpr View(View<T>&& value) = default;

    constexpr View(const View<T>& value) = default;

    ~View() = default;

    /// @brief 获取对象引用
    ATTR_PURE_INLINE
    constexpr T& Peer() const noexcept { return m_value.get(); }
    ATTR_PURE_INLINE
    constexpr T* NativeHandle() const noexcept { return &(m_value.get()); }

    // /// @brief 转换为 std::reference_wrapper<T>
    // ATTR_PURE_INLINE
    // constexpr operator std::reference_wrapper<T>() const noexcept {
    // 	return m_value;
    // }

 private:
    std::reference_wrapper<T> m_value;
};

// template<typename T>
// View<T>:: ~View()=default;
}  // namespace bus