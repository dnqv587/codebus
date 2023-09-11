#pragma once

#if defined __GNUC__ || defined __llvm__
	// @brief:   �������Ż�����������ʳ���
#	define LIKELY(x)      __builtin_expect(!!(x),1)
	// @brief:   �������Ż�����������ʲ�����
#	define UNLIKELY(x)    __builtin_expect(!!(x),0)
#else
#	define LIKELY(x)      (x)
#	define UNLIKELY(x)    (x)
#endif 
