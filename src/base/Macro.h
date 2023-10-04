#pragma once
//***********************************************
// name      : Macro.h
// brief     : 常用宏封装
// attention : None
// date      : 2023/9/15
//***********************************************

#if defined __GNUC__ || defined __llvm__
	/// @brief: 编译器优化，条件大概率成立
#	define LIKELY(x)      __builtin_expect(!!(x),1)
	/// @brief:编译器优化，条件大概率不成立
#	define UNLIKELY(x)    __builtin_expect(!!(x),0)
#else
#	define LIKELY(x)      (x)
#	define UNLIKELY(x)    (x)
#endif 
