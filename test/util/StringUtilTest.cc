//***********************************************
// name      : StringUtilTest.cc
// brief     : util 模块单元测试：被测 src/util/Util.h（StringUtil 命名空间）
// attention : BaseFileName 为 constexpr 模板（仅接受字符数组实参）
//***********************************************
#include <gtest/gtest.h>
#include "util/Util.h"

namespace bus
{
namespace test
{

TEST(StringUtilTest, BaseFileName)
{
    EXPECT_STREQ(StringUtil::BaseFileName("/path/to/file.cpp"), "file.cpp");
    EXPECT_STREQ(StringUtil::BaseFileName("file.cpp"), "file.cpp");
    EXPECT_STREQ(StringUtil::BaseFileName("/a/b/c/"), "");
}

TEST(StringUtilTest, StrToFileSize)
{
    EXPECT_EQ(StringUtil::StrToFileSize("4MB"), 4_MB);
    EXPECT_EQ(StringUtil::StrToFileSize("1GB"), 1_GB);
    EXPECT_EQ(StringUtil::StrToFileSize("5MB"), 5_MB);
    EXPECT_EQ(StringUtil::StrToFileSize("2TB"), 2_TB);
}

/// [正] KB 单位解析（1KB = 1024）
TEST(StringUtilTest, StrToFileSizeKB)
{
    EXPECT_EQ(StringUtil::StrToFileSize("1KB"), 1_KB);
    EXPECT_EQ(StringUtil::StrToFileSize("64KB"), 64_KB);
}

// ---------------------------------------------------------------------------
// 新增场景（P7）
// ---------------------------------------------------------------------------

/// [界] BaseFileName 斜杠变体：多斜杠、斜杠根、深层路径
TEST(StringUtilTest, BaseFileNameSlashVariants)
{
    EXPECT_STREQ(StringUtil::BaseFileName("/a//b//file.cc"), "file.cc");
    EXPECT_STREQ(StringUtil::BaseFileName("/"), "");
    EXPECT_STREQ(StringUtil::BaseFileName("/usr/local/include/stdio.h"), "stdio.h");
}

/// [界] StrToFileSize 数值边界与无单位输入的现状（锁定当前解析行为）
TEST(StringUtilTest, StrToFileSizeNumericBoundaries)
{
    EXPECT_EQ(StringUtil::StrToFileSize("0MB"), 0);
    EXPECT_EQ(StringUtil::StrToFileSize("1024MB"), 1024_MB);
    // 无单位输入返回数值部分（Q19 越界读已修复，行为保持）
    EXPECT_EQ(StringUtil::StrToFileSize("7"), 7);
}

} // namespace test
} // namespace bus
