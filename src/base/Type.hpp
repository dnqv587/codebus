#pragma once

typedef char Byte;

/// @brief 隐式类型转换
template<typename To,typename From>
inline To implicit_cast(const From& f)
{return f;}