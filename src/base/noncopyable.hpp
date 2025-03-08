#pragma once 

namespace bus
{
//***********************************************
// name      : noncopyable.hpp
// brief     : 不可拷贝类
// attention : None
// date      : 2023/9/15
//***********************************************
class noncopyable
{
 public:
	noncopyable() = default;
	~noncopyable() = default;
	noncopyable(const noncopyable&) = delete;
	noncopyable& operator=(const noncopyable&) = delete;
};
}