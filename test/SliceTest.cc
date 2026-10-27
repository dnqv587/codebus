//***********************************************
// name      : SliceTest.cc
// brief     : Slice 单元测试：被测 src/Buffer.hpp（Slice，只读字节视图）
// attention : 含 EXPECT_EXIT 死亡测试（需 Debug 构建，assert 未被 NDEBUG
//             关闭），sanitizer 构建下由守卫宏跳过
//***********************************************
#include <gtest/gtest.h>
#include "Buffer.hpp"
#include "TestHelper.h"

#include <csignal>
#include <cstring>
#include <sstream>
#include <string_view>
#include <vector>

namespace bus {
namespace test {

namespace {

std::vector<std::byte> MakeBytes(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

}  // namespace

TEST(SliceTest, DefaultConstructed) {
    Slice slice;
    EXPECT_EQ(slice.size(), 0U);
    EXPECT_TRUE(slice.empty());
    EXPECT_EQ(slice.data(), nullptr);
    EXPECT_EQ(slice.begin(), slice.end());
}

TEST(SliceTest, ConstructFromIterators) {
    std::vector<std::byte> bytes = MakeBytes("abcdef");
    Slice by_size(bytes.begin(), bytes.size());
    Slice by_range(bytes.begin(), bytes.end());

    EXPECT_EQ(by_size.size(), 6U);
    EXPECT_EQ(by_range.size(), 6U);
    EXPECT_EQ(by_size.toString(), "abcdef");
    EXPECT_EQ(by_range.toString(), "abcdef");
}

TEST(SliceTest, ConstructFromCharPointer) {
    char mutable_buf[] = {'a', 'b', '\0', 'c', 'd'};
    Slice mutable_slice(mutable_buf, sizeof(mutable_buf));
    EXPECT_EQ(mutable_slice.size(), 5U);
    EXPECT_EQ(mutable_slice.toString().size(), 5U);
    EXPECT_EQ(mutable_slice[0], std::byte{'a'});
    EXPECT_EQ(mutable_slice[2], std::byte{0});

    const char* const_buf = "hello";
    Slice const_slice(const_buf, 5);
    EXPECT_EQ(const_slice.toString(), "hello");
}

TEST(SliceTest, ConstructFromVoidPointer) {
    char buf[] = "void";
    void* mutable_ptr = buf;
    const void* const_ptr = buf;

    Slice mutable_slice(mutable_ptr, 4);
    Slice const_slice(const_ptr, 4);
    EXPECT_EQ(mutable_slice.toString(), "void");
    EXPECT_EQ(const_slice.toString(), "void");
}

TEST(SliceTest, ConstructFromStringView) {
    Slice normal(std::string_view("abc"));
    EXPECT_EQ(normal.size(), 3U);
    EXPECT_EQ(normal.toString(), "abc");

    Slice empty{std::string_view()};
    EXPECT_TRUE(empty.empty());

    const char raw[] = {'a', '\0', 'b', 'c'};
    Slice with_nul(std::string_view(raw, sizeof(raw)));
    EXPECT_EQ(with_nul.size(), 4U);
    EXPECT_EQ(with_nul.toString(), std::string_view(raw, sizeof(raw)));
}

TEST(SliceTest, SizeEmptyDataPeek) {
    std::vector<std::byte> bytes = MakeBytes("peek");
    Slice slice(bytes.begin(), bytes.size());

    EXPECT_EQ(slice.size(), bytes.size());
    EXPECT_FALSE(slice.empty());
    EXPECT_EQ(slice.peek().data(), slice.data());
    EXPECT_EQ(slice.peek().size(), slice.size());
}

TEST(SliceTest, IterationAndIndexing) {
    std::vector<std::byte> bytes = MakeBytes("xyz");
    Slice slice(bytes.begin(), bytes.size());

    EXPECT_EQ(std::distance(slice.begin(), slice.end()), 3);
    EXPECT_EQ(slice[0], std::byte{'x'});
    EXPECT_EQ(slice[1], std::byte{'y'});
    EXPECT_EQ(slice[2], std::byte{'z'});
}

TEST(SliceTest, ToStringKeepsEmbeddedNul) {
    const char raw[] = {'a', '\0', 'b'};
    Slice slice(raw, sizeof(raw));
    std::string_view text = slice.toString();
    EXPECT_EQ(text.size(), 3U);
    EXPECT_EQ(text, std::string_view(raw, sizeof(raw)));
}

TEST(SliceTest, StreamOutput) {
    Slice slice("stream");
    std::ostringstream os;
    os << slice;
    EXPECT_EQ(os.str(), "stream");
}

TEST(SliceTest, CopyIsShallowView) {
    char buf[] = "abc";
    Slice original(buf, 3);
    Slice copy = original;
    buf[0] = 'x';
    EXPECT_EQ(copy.toString(), "xbc");
    EXPECT_EQ(original.toString(), "xbc");
}

// ---------------------------------------------------------------------------
// 新增场景（P1）
// ---------------------------------------------------------------------------

/// [界] 零长度视图：覆盖真实存储的空 Slice，data() 仍指向存储而非空指针
TEST(SliceTest, ZeroLengthViewKeepsData) {
    std::vector<std::byte> bytes = MakeBytes("abc");
    Slice zero(bytes.begin(), 0);

    EXPECT_TRUE(zero.empty());
    EXPECT_EQ(zero.size(), 0U);
    EXPECT_EQ(zero.begin(), zero.end());
    EXPECT_EQ(zero.data(), reinterpret_cast<const std::byte*>(bytes.data()));
}

/// [界] 空指针 + 零长度构造存活（noexcept 契约）
TEST(SliceTest, NullPointerZeroLengthSurvives) {
    Slice from_const(static_cast<const void*>(nullptr), 0);
    EXPECT_TRUE(from_const.empty());
    EXPECT_EQ(from_const.data(), nullptr);

    Slice from_mutable(static_cast<void*>(nullptr), 0);
    EXPECT_TRUE(from_mutable.empty());
}

/// [错] operator[] 越界触发 assert（abort）——死亡测试
TEST(SliceTest, IndexOutOfBoundsAborts) {
    if (CODEBUS_TEST_UNDER_SANITIZER) {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            Slice slice("ab");
            (void)slice[5];
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

}  // namespace test
}  // namespace bus
