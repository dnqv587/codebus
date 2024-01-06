#pragma once
#include <functional>
#include <type_traits>

//***********************************************
// class     : View
// brief     : 视图基类，不拥有对象生存周期
// attention : None
//***********************************************
template<typename T>
class View
{
 public:
    explicit View(std::reference_wrapper<T> value)
    :m_value(value)
    {}

	virtual ~View()=0;

    /// @brief 获取对象引用
    [[nodiscard]]
    T& peer() noexcept
    {
        return m_value.get();
    }

    /// @brief 拷贝视图对象
    /// @return 如果对象不可拷贝，返回初始对象
    [[nodiscard]]
    T copy() const noexcept
    {
        if( typename std::is_copy_constructible<T>::value() ==  true)
        {
            return T{m_value};
        }
        else
        {
            T{};
        }
    }

protected:
    std::reference_wrapper<T> m_value;
};

template<typename T>
View<T>:: ~View()=default;