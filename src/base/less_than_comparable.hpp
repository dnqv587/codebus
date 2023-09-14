#pragma once

//***********************************************
// name      : less_than_comparable.hpp
// brief     : >,>=.<= 操作符重载实现类
// attention : 派生类需实现 operator<
// date      : 2023/9/15
//***********************************************
template<typename T>
class less_than_comparable
{
public:
    friend bool operator >(const T& lhs,const T& rhs)
    {return rhs < lhs;}
    friend bool operator>=(const T& lhs,const T& rhs)
    {return static_cast<bool>(!(lhs<rhs));}
    friend bool operator<=(const T& lhs,const T& rhs)
    {return static_cast<bool>(!(rhs<lhs));}
};