//***********************************************
// name      : BufferTest.cc
// brief     : Buffer 单元测试：被测 src/Buffer.hpp（Buffer，三区读写缓冲）
// attention : 含 EXPECT_EXIT 死亡测试（需 Debug 构建，assert 未被 NDEBUG
//             关闭），sanitizer 构建下由守卫宏跳过
//***********************************************
#include <gtest/gtest.h>
#include "Buffer.hpp"
#include "TestHelper.h"

#include <algorithm>
#include <csignal>
#include <cstring>
#include <random>
#include <string_view>
#include <vector>

namespace bus {
namespace test {

namespace {

bool ContentEquals(const Slice& slice, const std::vector<std::byte>& expected) {
    return slice.size() == expected.size() && std::equal(slice.begin(), slice.end(), expected.begin());
}

std::vector<std::byte> MakeBytes(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::vector<std::byte> PeekedBytes(Buffer& buffer) {
    Slice slice = buffer.PeekAll();
    return std::vector<std::byte>(slice.begin(), slice.end());
}

}  // namespace

TEST(BufferTest, ConstructorInvariants) {
    Buffer buffer(16);
    EXPECT_EQ(buffer.ReadableSize(), 0U);
    EXPECT_EQ(buffer.WritableSize(), 16U);
    EXPECT_EQ(buffer.Capacity(), 16U + Buffer::CheapPrependSize);
    EXPECT_EQ(buffer.PrependableSize(), Buffer::CheapPrependSize);
}

/// [界] 零容量缓冲：WritableSize()==0 合法，写入触发增长
TEST(BufferTest, ZeroCapacity) {
    Buffer buffer(0);
    EXPECT_EQ(buffer.WritableSize(), 0U);

    buffer.Write(Slice("x"));
    EXPECT_EQ(buffer.ReadableSize(), 1U);
    EXPECT_EQ(buffer.PeekAll().toString(), "x");
}

TEST(BufferTest, WriteAndPeek) {
    Buffer buffer(16);
    buffer.Write(Slice("hello"));

    EXPECT_EQ(buffer.ReadableSize(), 5U);
    EXPECT_EQ(buffer.WritableSize(), 11U);
    EXPECT_EQ(buffer.PeekAll().toString(), "hello");
}

TEST(BufferTest, WriteEmptySlice) {
    Buffer buffer(16);
    const size_t before_writable = buffer.WritableSize();
    const size_t before_readable = buffer.ReadableSize();

    buffer.Write(Slice());

    EXPECT_EQ(buffer.ReadableSize(), before_readable);
    EXPECT_EQ(buffer.WritableSize(), before_writable);
    EXPECT_EQ(buffer.PrependableSize(), Buffer::CheapPrependSize);
}

TEST(BufferTest, WriteExactCapacity) {
    Buffer buffer(4);
    const size_t capacity = buffer.Capacity();

    buffer.Write(Slice("abcd"));

    EXPECT_EQ(buffer.Capacity(), capacity);
    EXPECT_EQ(buffer.ReadableSize(), 4U);
    EXPECT_EQ(buffer.PeekAll().toString(), "abcd");
}

TEST(BufferTest, WriteTriggersGrowth) {
    // 注意：恰好写满为合法状态（D13 已闭环，WritableSize 断言 >=）
    Buffer buffer(4);
    buffer.Write(Slice("abc"));  // 剩余可写 1 字节
    const size_t capacity_before = buffer.Capacity();

    buffer.Write(Slice("de"));  // 空间不足，触发扩容

    EXPECT_GT(buffer.Capacity(), capacity_before);
    EXPECT_EQ(buffer.ReadableSize(), 5U);
    EXPECT_EQ(buffer.PeekAll().toString(), "abcde");
}

TEST(BufferTest, WriteLargeData) {
    constexpr size_t kSize = 1024 * 1024;
    std::vector<std::byte> payload(kSize);
    for (size_t i = 0; i < kSize; ++i) {
        payload[i] = std::byte{static_cast<unsigned char>(i % 251)};
    }

    Buffer buffer(1024);
    buffer.Write(Slice(payload.begin(), payload.size()));

    EXPECT_EQ(buffer.ReadableSize(), kSize);
    EXPECT_TRUE(ContentEquals(buffer.PeekAll(), payload));
}

TEST(BufferTest, PrependHeader) {
    Buffer buffer(16);
    buffer.Write(Slice("body"));
    buffer.Prepend(Slice("H:"));

    EXPECT_EQ(buffer.ReadableSize(), 6U);
    EXPECT_EQ(buffer.PeekAll().toString(), "H:body");
}

TEST(BufferTest, PrependAfterRead) {
    Buffer buffer(16);
    buffer.Write(Slice("01234567"));
    buffer.Read(8);

    EXPECT_GE(buffer.PrependableSize(), 8U);
    buffer.Prepend(Slice("HEADER12"));
    EXPECT_EQ(buffer.PeekAll().toString(), "HEADER12");
}

TEST(BufferTest, ReadPartial) {
    Buffer buffer(16);
    buffer.Write(Slice("abcdef"));

    Slice front = buffer.Read(3);

    EXPECT_EQ(front.toString(), "abc");
    EXPECT_EQ(buffer.ReadableSize(), 3U);
    EXPECT_EQ(buffer.PeekAll().toString(), "def");
}

TEST(BufferTest, ReadAllData) {
    Buffer buffer(16);
    buffer.Write(Slice("abc"));

    Slice all = buffer.Read(10);

    EXPECT_EQ(all.toString(), "abc");
    EXPECT_EQ(buffer.ReadableSize(), 0U);
}

TEST(BufferTest, ReadZero) {
    Buffer buffer(16);
    buffer.Write(Slice("abc"));

    Slice empty = buffer.Read(0);

    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(buffer.ReadableSize(), 3U);
}

TEST(BufferTest, ReadAllThenReset) {
    Buffer buffer(16);
    buffer.Write(Slice("abc"));
    buffer.ReadAll();

    EXPECT_EQ(buffer.ReadableSize(), 0U);
    EXPECT_EQ(buffer.PrependableSize(), Buffer::CheapPrependSize);
}

TEST(BufferTest, PeekDoesNotConsume) {
    Buffer buffer(16);
    buffer.Write(Slice("abcd"));

    EXPECT_EQ(buffer.Peek(2).toString(), "ab");
    EXPECT_EQ(buffer.ReadableSize(), 4U);
    EXPECT_EQ(buffer.PeekAll().toString(), "abcd");
    EXPECT_EQ(buffer.ReadableSize(), 4U);
}

TEST(BufferTest, SkipReadAndWrite) {
    Buffer buffer(16);
    buffer.Write(Slice("abcd"));

    buffer.SkipRead(2);
    EXPECT_EQ(buffer.ReadableSize(), 2U);
    EXPECT_EQ(buffer.PeekAll().toString(), "cd");

    buffer.SkipWrite(2);
    EXPECT_EQ(buffer.ReadableSize(), 4U);
}

TEST(BufferTest, TruncateKeepPrefix) {
    Buffer buffer(16);
    buffer.Write(Slice("abcdef"));

    buffer.Truncate(3);

    EXPECT_EQ(buffer.ReadableSize(), 3U);
    EXPECT_EQ(buffer.PeekAll().toString(), "abc");
}

TEST(BufferTest, TruncateZeroResets) {
    Buffer buffer(16);
    buffer.Write(Slice("abcdef"));
    buffer.Truncate(0);

    EXPECT_EQ(buffer.ReadableSize(), 0U);
    EXPECT_EQ(buffer.PrependableSize(), Buffer::CheapPrependSize);

    buffer.Write(Slice("xy"));
    EXPECT_EQ(buffer.PeekAll().toString(), "xy");
}

TEST(BufferTest, FindEolFound) {
    Buffer buffer(16);
    buffer.Write(Slice("ab\ncd"));

    auto eol = buffer.FindEOL();

    ASSERT_NE(eol, buffer.end());
    EXPECT_EQ(*eol, std::byte{'\n'});
    EXPECT_EQ(std::distance(buffer.ReadBegin(), eol), 2);
}

TEST(BufferTest, FindEolNotFound) {
    Buffer buffer(16);
    buffer.Write(Slice("abcd"));

    // 未找到时返回可读区末尾（WriteBegin）
    EXPECT_EQ(buffer.FindEOL(), buffer.WriteBegin());
}

TEST(BufferTest, FindEolBoundaries) {
    Buffer buffer(16);
    buffer.Write(Slice("\nabc"));
    EXPECT_EQ(buffer.FindEOL(), buffer.ReadBegin());
    buffer.Reset();

    buffer.Write(Slice("abc\n"));
    EXPECT_EQ(std::distance(buffer.ReadBegin(), buffer.FindEOL()), 3);
    buffer.Reset();

    buffer.Write(Slice("a\nb\nc"));
    EXPECT_EQ(std::distance(buffer.ReadBegin(), buffer.FindEOL()), 1);
    buffer.Reset();

    buffer.Write(Slice("abc"));
    EXPECT_EQ(buffer.FindEOL(), buffer.WriteBegin());
}

TEST(BufferTest, FindEolIgnoresNonReadableData) {
    Buffer buffer(16);
    buffer.Write(Slice("x\n"));
    buffer.ReadAll();  // 数据不再可读

    EXPECT_EQ(buffer.ReadableSize(), 0U);
    EXPECT_EQ(buffer.FindEOL(), buffer.WriteBegin());
}

TEST(BufferTest, EnsureWriteableSize) {
    Buffer buffer(4);
    const size_t capacity_before = buffer.Capacity();

    buffer.EnsureWriteableSize(10);

    EXPECT_GE(buffer.WritableSize(), 10U);
    EXPECT_GE(buffer.Capacity(), capacity_before);
}

TEST(BufferTest, SlidingWindowCompaction) {
    Buffer buffer(16);
    const size_t capacity_before = buffer.Capacity();

    buffer.Write(Slice("0123456789ab"));  // 写入 12 字节
    buffer.Read(8);                       // 剩 "89ab"，释放预留区
    buffer.Write(Slice("CDEFGHIJ"));      // 触发滑动窗口而非扩容

    EXPECT_EQ(buffer.Capacity(), capacity_before);
    EXPECT_EQ(buffer.PrependableSize(), Buffer::CheapPrependSize);
    EXPECT_EQ(buffer.PeekAll().toString(), "89abCDEFGHIJ");
}

TEST(BufferTest, CopySemantics) {
    Buffer source(16);
    source.Write(Slice("abc"));

    Buffer copy = source;
    copy.Write(Slice("def"));

    EXPECT_EQ(source.PeekAll().toString(), "abc");
    EXPECT_EQ(copy.PeekAll().toString(), "abcdef");
}

TEST(BufferTest, RandomizedModelTest) {
    std::mt19937 engine(20261017U);
    std::uniform_int_distribution<int> op_dist(0, 5);
    std::uniform_int_distribution<int> len_dist(0, 64);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    Buffer buffer(8);
    std::vector<std::byte> model;

    for (int step = 0; step < 500; ++step) {
        const int op = op_dist(engine);
        switch (op) {
            case 0:  // Write
            {
                size_t len = static_cast<size_t>(len_dist(engine));
                std::vector<std::byte> payload(len);
                for (auto& b : payload) {
                    b = std::byte{static_cast<unsigned char>(byte_dist(engine))};
                }
                buffer.Write(Slice(payload.begin(), payload.size()));
                model.insert(model.end(), payload.begin(), payload.end());
                break;
            }
            case 1:  // Read
            {
                const size_t len = static_cast<size_t>(len_dist(engine));
                const size_t expect_len = std::min(len, model.size());
                Slice read = buffer.Read(len);
                ASSERT_EQ(read.size(), expect_len);
                ASSERT_TRUE(ContentEquals(read, std::vector<std::byte>(model.begin(), model.begin() + expect_len)));
                model.erase(model.begin(), model.begin() + expect_len);
                break;
            }
            case 2:  // Peek
            {
                const size_t len = static_cast<size_t>(len_dist(engine));
                const size_t expect_len = std::min(len, model.size());
                Slice peeked = buffer.Peek(len);
                ASSERT_EQ(peeked.size(), expect_len);
                ASSERT_TRUE(ContentEquals(peeked, std::vector<std::byte>(model.begin(), model.begin() + expect_len)));
                break;
            }
            case 3:  // SkipRead
            {
                const size_t len = std::min(static_cast<size_t>(len_dist(engine)), model.size());
                buffer.SkipRead(len);
                model.erase(model.begin(), model.begin() + len);
                break;
            }
            case 4:  // Truncate
            {
                const size_t len = std::min(static_cast<size_t>(len_dist(engine)), model.size());
                buffer.Truncate(len);
                model.resize(len);
                break;
            }
            default:  // Reset
            {
                buffer.Reset();
                model.clear();
                break;
            }
        }

        ASSERT_EQ(buffer.ReadableSize(), model.size()) << "step=" << step;
        ASSERT_TRUE(ContentEquals(buffer.PeekAll(), model)) << "step=" << step;
    }
}

/// [正] Reserve 扩容后 Capacity/WritableSize 同步更新
TEST(BufferTest, ReserveUpdatesCapacity) {
    Buffer buffer(4);
    buffer.Reserve(1024);

    EXPECT_GE(buffer.Capacity(), 1024U + Buffer::CheapPrependSize);
    EXPECT_GE(buffer.WritableSize(), 1024U);

    buffer.Write(Slice("abc"));
    EXPECT_EQ(buffer.PeekAll().toString(), "abc");
}

// ---------------------------------------------------------------------------
// 新增场景（P1）
// ---------------------------------------------------------------------------

/// [压] 随机模型多种子长程对照：3 个固定种子 × 2000 步，失败信息携带种子与步号
/// @attention 多种子重复 RandomizedModelTest 的模型（含恰好写满路径，D13 已闭环）
TEST(BufferTest, RandomizedModelMultiSeed) {
    const unsigned seeds[] = {20261017U, 42U, 7U};

    for (const unsigned seed : seeds) {
        std::mt19937 engine(seed);
        std::uniform_int_distribution<int> op_dist(0, 5);
        std::uniform_int_distribution<int> len_dist(0, 64);
        std::uniform_int_distribution<int> byte_dist(0, 255);

        Buffer buffer(8);
        std::vector<std::byte> model;

        for (int step = 0; step < 2000; ++step) {
            const int op = op_dist(engine);
            switch (op) {
                case 0:  // Write
                {
                    size_t len = static_cast<size_t>(len_dist(engine));
                    std::vector<std::byte> payload(len);
                    for (auto& b : payload) {
                        b = std::byte{static_cast<unsigned char>(byte_dist(engine))};
                    }
                    buffer.Write(Slice(payload.begin(), payload.size()));
                    model.insert(model.end(), payload.begin(), payload.end());
                    break;
                }
                case 1:  // Read
                {
                    const size_t len = static_cast<size_t>(len_dist(engine));
                    const size_t expect_len = std::min(len, model.size());
                    Slice read = buffer.Read(len);
                    ASSERT_EQ(read.size(), expect_len) << "seed=" << seed << " step=" << step;
                    ASSERT_TRUE(ContentEquals(read, std::vector<std::byte>(model.begin(), model.begin() + expect_len)))
                        << "seed=" << seed << " step=" << step;
                    model.erase(model.begin(), model.begin() + expect_len);
                    break;
                }
                case 2:  // Peek
                {
                    const size_t len = static_cast<size_t>(len_dist(engine));
                    const size_t expect_len = std::min(len, model.size());
                    Slice peeked = buffer.Peek(len);
                    ASSERT_EQ(peeked.size(), expect_len) << "seed=" << seed << " step=" << step;
                    ASSERT_TRUE(
                        ContentEquals(peeked, std::vector<std::byte>(model.begin(), model.begin() + expect_len)))
                        << "seed=" << seed << " step=" << step;
                    break;
                }
                case 3:  // SkipRead
                {
                    const size_t len = std::min(static_cast<size_t>(len_dist(engine)), model.size());
                    buffer.SkipRead(len);
                    model.erase(model.begin(), model.begin() + len);
                    break;
                }
                case 4:  // Truncate
                {
                    const size_t len = std::min(static_cast<size_t>(len_dist(engine)), model.size());
                    buffer.Truncate(len);
                    model.resize(len);
                    break;
                }
                default:  // Reset
                {
                    buffer.Reset();
                    model.clear();
                    break;
                }
            }

            ASSERT_EQ(buffer.ReadableSize(), model.size()) << "seed=" << seed << " step=" << step;
            ASSERT_TRUE(ContentEquals(buffer.PeekAll(), model)) << "seed=" << seed << " step=" << step;
        }
    }
}

/// [界] Prepend 超过 PrependableSize 触发 assert（abort）——死亡测试
TEST(BufferTest, PrependBeyondPrependableAborts) {
    if (CODEBUS_TEST_UNDER_SANITIZER) {
        GTEST_SKIP() << "death test skipped under sanitizer";
    }
    EXPECT_EXIT(
        {
            Buffer buffer(16);
            buffer.Write(Slice("body"));  // read_idx = CheapPrependSize(8)
            // 预留区仅 8 字节，Prepend 9 字节越界
            buffer.Prepend(Slice("123456789"));
        },
        ::testing::KilledBySignal(SIGABRT), ".*");
}

}  // namespace test

/// [界] Q27/D51：真正进入 grow 滑动分支（H2 审查修正：原用例容量计算错误
/// 从未触发 grow——空转锁定）。read==CheapPrependSize 的同位场景在正常算术下
/// 不可达（需 Writable<len≤Writable，矛盾）——仅 D51 的 readv 回绕路径可达，
/// 已从 Socket 层源头修复
TEST(BufferTest, GrowSlideAtCheapPrependBoundary) {
    // 滑动分支：read_idx(12) > CheapPrependSize(8)，数据左移
    Buffer buf(16);                      // cap = 16+8 = 24
    buf.Write(Slice("0123456789ab", 12));  // read=8, write=20
    buf.SkipRead(4);                     // read=12, write=20
    buf.EnsureWriteableSize(8);          // Writable=4<8 且 4+12>=8+8 → 滑动
    EXPECT_EQ(buf.ReadableSize(), 8u);
    EXPECT_EQ(std::string(buf.Peek(8).toString()), "456789ab");

    // 右移场景：Prepend 后 read_idx < CheapPrependSize，move_backward 安全右移
    Buffer buf2(20);                     // cap=28
    buf2.Write(Slice("abcdefgh", 8));    // read=8, write=16
    buf2.Prepend(Slice("12", 2));        // read=6（右移窗口）
    buf2.EnsureWriteableSize(12);        // Writable=12>=12 不触发；再压满
    buf2.Write(Slice("wxyz", 4));        // read=6, write=20
    buf2.EnsureWriteableSize(8);         // Writable=8>=8 不触发；构造 Writable<8：
    (void)buf2;
    Buffer buf3(20);
    buf3.Write(Slice("abcdefgh", 8));
    buf3.Prepend(Slice("1234", 4));      // read=4
    buf3.Write(Slice("ijkl", 4));        // write=20，Writable=8
    buf3.EnsureWriteableSize(10);        // 8<10 且 8+4=12 < 10+8=18 → resize（非滑动）
    EXPECT_EQ(buf3.ReadableSize(), 16u);  // 4(prepend)+8+4(append)
    EXPECT_EQ(std::string(buf3.Peek(16).toString()), "1234abcdefghijkl");
}

}  // namespace bus

