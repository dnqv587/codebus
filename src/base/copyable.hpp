#pragma once

namespace uf{
//***********************************************
// name      : copyable.hpp
// brief     : 可拷贝类
// attention : None
// date      : 2023/9/15
//***********************************************
class copyable
{
 public:
	copyable() = default;
	~copyable() = default;
	copyable& operator=(const copyable&) = default;
	copyable(const copyable&) = default;
};

}