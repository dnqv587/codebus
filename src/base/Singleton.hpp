#pragma once
#include <base/noncopyable.hpp>

namespace bus
{
template<typename T>
class Singleton : noncopyable
{
 public:
	Singleton() = delete;
	~Singleton() = delete;

	static T* getInstance() noexcept
	{
		static T v;
		return &v;
	}
};
}