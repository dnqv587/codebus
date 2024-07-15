#pragma once
#include <cstdio>
#include <cstdlib>

namespace uf{
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

/// @brief: 常量参数转换
#define CONSTEVAL(x)  []()->decltype(auto){return (x);}
#define GET_CONSTEVAL(x)   (x)()

static void assert_fail(const char* assertion, const char* file, unsigned int line, const char* msg)
{
	(void)::fprintf(stderr, "in %s:%u Assertion '%s' failed,%s \n", file, line, assertion, msg);
	fflush(stderr);
	abort();
}
/// @brief:断言
#define ASSERT(expr, msg) \
            static_cast<bool>(expr)?void(0):assert_fail (#expr, __FILE__, __LINE__, msg)


/// @brief 属性
#if defined __GNUC__ || defined __llvm__
#define ATTRIBUTE(...)  __attribute__((__VA_ARGS__))
#else
#define ATTR(...)
#endif

/// @brief [func]强制内联
#if defined __GNUC__ || defined __llvm__
#define ATTR_INLINE  ATTRIBUTE(__always_inline__) inline
#else
#define ATTR_INLINE
#endif

/// @brief [func]返回值检测
#if defined __GNUC__ || defined __llvm__
#if __cplusplus >= 17
#define ATTR_NODISCARD [[nodiscard]]
#else
#define ATTR_NODISCARD ATTRIBUTE(__warn_unused_result__)
#endif
#else
#define ATTR_NODISCARD
#endif

/// @brief [func]声明该函数只返回一个值外没有其他效果，返回值只依赖于参数和全局变量
#ifdef NDEBUG
#define ATTR_PURE ATTRIBUTE(pure)
#else
#define ATTR_PURE
#endif

/// @brief [func]声明该函数不读取或修改任何全局内存,该函数只检查它的参数，除了返回值之外没有其他效果
#ifdef NDEBUG
#define ATTR_CONST ATTRIBUTE(const)
#else
#define ATTR_CONST
#endif

/// @brief [func]声明函数为getter类型。
/// 函数属性为：内联；返回值警告；只返回一个值，返回值只依赖于参数和全局变量；
#if defined __GNUC__ || defined __llvm__
#define ATTR_PURE_INLINE \
                         ATTR_NODISCARD\
                         ATTR_INLINE \
                         ATTR_PURE
#else
#define ATTR_PURE_INLINE
#endif


/// @brief [func]声明函数为getter类型。与pure类型区别为返回值不依赖全局变量
/// 函数属性为：内联；返回值警告；只返回一个值，返回值只依赖于参数
#if defined __GNUC__ || defined __llvm__
#define ATTR_CONST_INLINE \
                         ATTR_INLINE \
                         ATTR_NODISCARD\
                         ATTR_PURE
#else
#define ATTR_CONST_INLINE
#endif

/// @brief [func] 字符串格式化检查
/// @arg type 按照printf, scanf, strftime或strfmon的参数表格式规则对该函数的参数进行检查
/// @arg fmt_idx 格式化字符串参数下标
/// @arg arg_idx 格式化参数开始下标
#if defined __GNUC__ || defined __llvm__
#define  ARRT_FORMAT(type, fmt_idx, arg_idx) ATTRIBUTE(format(type,fmt_idx,arg_idx))
#else
#define  ARRT_FORMAT(type,fmt_idx,arg_idx)
#endif

//clang的线程安全注解--需加入编译选项 -Wthread-safety
// https://clang.llvm.org/docs/ThreadSafetyAnalysis.html

#if defined(__clang__) && (!defined(SWIG))
#define THREAD_ANNOTATION_ATTRIBUTE__(x)   __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x)   // no-op
#endif

//CAPABILITY 表明某个类对象可以当作 capability 使⽤，其中 x 的类型是 string，能够在错误信息当中指出对应的 capability 的名称
#define CAPABILITY(x) \
  THREAD_ANNOTATION_ATTRIBUTE__(capability(x))

//SCOPED_CAPABILITY ⽤于修饰基于 RAII 实现的 capability
#define SCOPED_CAPABILITY \
  THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)
//end

//GUARD_BY ⽤于修饰对象，表明该对象需要受到 capability 的保护
#define GUARDED_BY(x) \
  THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))

//PT_GUARDED_BY⽤于修饰指针类型变量，在更改指针变量所指向的内容前需要加锁，否则发出警告
#define PT_GUARDED_BY(x) \
  THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))

//ACQUIRED_BEFORE 和 ACQUIRED_AFTER 主要⽤于修饰 capability 的获取顺序，⽤于避免死锁
#define ACQUIRED_BEFORE(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))

#define ACQUIRED_AFTER(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))

//REQUIRES 声明调⽤线程必须拥有对指定的 capability 具有独占访问权。可以指定多个 capabilities。函数/⽅法在访问资源时，必须先上锁，再调⽤函数，然后再解锁
#define REQUIRES(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(requires_capability(__VA_ARGS__))

//REQUIRES_SHARED 功能与 REQUIRES 相同，但是可以共享访问
#define REQUIRES_SHARED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(requires_shared_capability(__VA_ARGS__))

//CQUIRE 表⽰⼀个函数/⽅法需要持有⼀个 capability，但并不释放这个 capability。调⽤者在调⽤被 ACQUIRE 修饰的函数/⽅法时，要确保没有持有任何 capability，进入函数时才会持有capability
#define ACQUIRE(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))

//ACQUIRE_SHARED 与 ACQUIRE 的功能是类似的，但持有的是共享的 capability
#define ACQUIRE_SHARED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(acquire_shared_capability(__VA_ARGS__))

//RELEASE 和 RELEASE_SHARED 与 ACQUIRE 和 ACQUIRE_SHARED 正相反，它们表⽰调⽤⽅在调⽤该函数/⽅法时需要先持有锁，⽽当函数执⾏结束后会释放锁
#define RELEASE(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))

#define RELEASE_SHARED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(release_shared_capability(__VA_ARGS__))

#define TRY_ACQUIRE(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_capability(__VA_ARGS__))

#define TRY_ACQUIRE_SHARED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_shared_capability(__VA_ARGS__))

//EXCLUDES ⽤于显式声明函数/⽅法不应该持有某个特定的 capability。由于 mutex 的实现通常是不可重⼊的，因此 EXCLUDES 通常被⽤来预防死锁
#define EXCLUDES(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))

//ASSERT_* 表⽰在运⾏时检测调⽤线程是否持有 capability
#define ASSERT_CAPABILITY(x) \
  THREAD_ANNOTATION_ATTRIBUTE__(assert_capability(x))

#define ASSERT_SHARED_CAPABILITY(x) \
  THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_capability(x))

//RETURN_CAPABILITY 通常⽤于修饰那些被当作 capability getter 的函数，这些函数会返回 capability 的引⽤或指针
#define RETURN_CAPABILITY(x) \
  THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))

//NO_THREAD_SAFETY_ANALYSIS 表⽰关闭某个函数/⽅法的 TSA 检测，通常只⽤于两种情况：1，该函数/⽅法可以被做成⾮线程安全；2、函数/⽅法太过复杂
#define NO_THREAD_SAFETY_ANALYSIS \
  THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

}
