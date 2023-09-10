#pragma once 

//************************************
// class:  noncopyable
// brief:  派生类不可被拷贝
//************************************
class noncopyable
{
public:
	noncopyable() = default;
	~noncopyable() = default;
	noncopyable(const noncopyable&) = delete;
	noncopyable& operator=(const noncopyable&) = delete;
};
