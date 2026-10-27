//***********************************************
// name      : TypeTest.cc
// brief     : 根级基础设施模块契约测试：被测 src/type.h（字面量/implicit_cast/is_one_of）
//             与 copyable/noncopyable/less_than_comparable 基类契约
// attention : 以编译期 static_assert 为主；为保证目标非空跑，至少含一个
//             运行时断言用例（gtest_main 不接受全静态目标）
//***********************************************
#include <gtest/gtest.h>
#include "copyable.hpp"
#include "less_than_comparable.hpp"
#include "noncopyable.hpp"
#include "type.h"

#include <sys/types.h>

#include <type_traits>

namespace bus {
namespace test {

namespace {

struct CopyableValue : copyable {
    int value{0};
};

struct Entity : noncopyable {
    int value{0};
};

struct Comparable : copyable, less_than_comparable<Comparable> {
    explicit Comparable(int v) : value(v) {}

    int value{0};

    friend bool operator<(const Comparable& lhs, const Comparable& rhs) {
        return lhs.value < rhs.value;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 编译期契约（static_assert，编译通过即通过）
// ---------------------------------------------------------------------------

static_assert(std::is_same_v<decltype(1_KB), off_t>, "1_KB 应为 off_t");
static_assert(std::is_same_v<decltype(1_MB), off_t>, "1_MB 应为 off_t");
static_assert(std::is_same_v<decltype(1_GB), off64_t>, "1_GB 应为 off64_t");
static_assert(std::is_same_v<decltype(1_TB), off64_t>, "1_TB 应为 off64_t");

static_assert(std::is_same_v<decltype(implicit_cast<int>(42)), int>, "implicit_cast 应可显式指明目标类型");

static_assert(is_one_of<int, int, long>, "int 属于 {int, long}");
static_assert(!is_one_of<int, long, double>, "int 不属于 {long, double}");

static_assert(std::is_copy_constructible_v<CopyableValue>, "copyable 派生类应可拷贝");
static_assert(!std::is_copy_constructible_v<Entity>, "noncopyable 派生类应不可拷贝");
static_assert(!std::is_move_constructible_v<Entity>, "noncopyable 派生类应不可移动");
static_assert(std::is_default_constructible_v<Entity>, "noncopyable 不影响默认构造");

// ---------------------------------------------------------------------------
// 运行时断言
// ---------------------------------------------------------------------------

/// [正] 文件大小字面量的数值契约
TEST(TypeTest, FileSizeLiterals) {
    EXPECT_EQ(1_KB, 1024);
    EXPECT_EQ(1_MB, 1024 * 1024);
    EXPECT_EQ(1_GB, 1024LL * 1024 * 1024);
    EXPECT_EQ(1_TB, 1024LL * 1024 * 1024 * 1024);

    EXPECT_EQ(4_MB, 4 * 1024 * 1024);
    EXPECT_EQ(128_MB, 128 * 1024 * 1024);
}

/// [正] is_one_of 的真/假判定
TEST(TypeTest, IsOneOf) {
    EXPECT_TRUE((is_one_of<int, int>));
    EXPECT_TRUE((is_one_of<int, int, double, long>));
    EXPECT_FALSE((is_one_of<int, double>));
    EXPECT_FALSE((is_one_of<void>));
}

/// [正] implicit_cast 显式指明目标类型
TEST(TypeTest, ImplicitCast) {
    EXPECT_EQ(implicit_cast<int>(42), 42);
    const double source = 3.5;
    EXPECT_DOUBLE_EQ(implicit_cast<double>(source), 3.5);
}

/// [正] less_than_comparable 由 operator< 导出全部序运算符
TEST(TypeTest, LessThanComparableDerivesOperators) {
    Comparable small(1);
    Comparable big(2);

    EXPECT_TRUE(small < big);
    EXPECT_TRUE(big > small);
    EXPECT_TRUE(small <= big);
    EXPECT_TRUE(big >= small);
    EXPECT_TRUE(small <= Comparable(1));
    EXPECT_TRUE(small >= Comparable(1));
}

}  // namespace test
}  // namespace bus
