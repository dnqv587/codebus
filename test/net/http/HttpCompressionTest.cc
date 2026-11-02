//***********************************************
// name      : HttpCompressionTest.cc
// brief     : gzip 纯函数层测试（压缩往返/zlib bomb 防护/MIME 分类）
// attention : None
//***********************************************
#include <gtest/gtest.h>

#include <string>

#include "net/http/HttpCompression.h"
#include "type.h"

using namespace bus;
using namespace bus::http;

namespace {

/// @brief 生成重复内容串（高压缩比）
std::string MakeRepeat(size_t n, char c = 'x') {
    return std::string(n, c);
}

}  // namespace

/// [正] 压缩解压往返（多尺寸）
TEST(HttpCompressionTest, CompressDecompressRoundTrip) {
    for (size_t n : std::vector<size_t>{1u, 100u, size_t(1_KB), size_t(100_KB)}) {
        std::string raw = MakeRepeat(n, 'a' + (n % 26));
        std::string packed;
        ASSERT_TRUE(GzipCompress(raw, packed, -1)) << " n=" << n;
        if (n > 64) {
            EXPECT_LT(packed.size(), raw.size()) << " n=" << n;  // 重复内容应显著缩小
        }
        std::string back;
        EXPECT_EQ(GzipDecompress(packed, back, 1_MB), 0);
        EXPECT_EQ(back, raw) << " n=" << n;
    }
}

/// [界] zlib bomb：高压缩比解压超上限 → -2（防护锁定）
TEST(HttpCompressionTest, DecompressBombCapped) {
    std::string packed;
    ASSERT_TRUE(GzipCompress(MakeRepeat(1_MB), packed, 9));
    ASSERT_LT(packed.size(), size_t(2_KB));  // 确属高压缩比
    std::string out;
    EXPECT_EQ(GzipDecompress(packed, out, 16_KB), -2);  // 超 16KB 上限拒绝
}

/// [界] 非 gzip 数据 → -1
TEST(HttpCompressionTest, DecompressGarbageRejected) {
    std::string out;
    EXPECT_EQ(GzipDecompress("not gzip data at all", out, size_t(1_KB)), -1);
}

/// [界] 截断的 gzip 流 → -1
TEST(HttpCompressionTest, DecompressTruncatedRejected) {
    std::string packed;
    ASSERT_TRUE(GzipCompress(MakeRepeat(size_t(10_KB)), packed, -1));
    std::string out;
    EXPECT_EQ(GzipDecompress(packed.substr(0, packed.size() / 2), out, size_t(1_MB)), -1);
}

/// [正] MIME 可压缩分类
TEST(HttpCompressionTest, MimeClassification) {
    EXPECT_TRUE(IsCompressibleMime("text/html"));
    EXPECT_TRUE(IsCompressibleMime("text/html; charset=utf-8"));
    EXPECT_TRUE(IsCompressibleMime("application/json"));
    EXPECT_TRUE(IsCompressibleMime("application/javascript"));
    EXPECT_TRUE(IsCompressibleMime("application/xml"));
    EXPECT_TRUE(IsCompressibleMime("image/svg+xml"));
    EXPECT_FALSE(IsCompressibleMime("image/png"));
    EXPECT_FALSE(IsCompressibleMime("application/octet-stream"));
    EXPECT_FALSE(IsCompressibleMime(""));
}
