#pragma once 

//************************************
// class:  noncopyable
// brief:  �����಻�ɱ�����
//************************************
class noncopyable
{
public:
	noncopyable() = default;
	~noncopyable() = default;
	noncopyable(const noncopyable&) = delete;
	noncopyable& operator=(const noncopyable&) = delete;
};
