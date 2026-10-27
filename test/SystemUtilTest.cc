//***********************************************
// name      : SystemUtilTest.cc
// brief     : 根级工具模块单元测试：被测 src/Util.h（SystemUtil 命名空间）
// attention : Abort() 会 core dump 并打断调试体验，不做用例（如需回归以
//             独立 EXPECT_DEATH 目标补充）；getWorkId 当前硬编码返回 1
//***********************************************
#include <gtest/gtest.h>
#include "Util.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bus {
namespace test {

TEST(SystemUtilTest, CoreNum) { EXPECT_GT(SystemUtil::getCoreNum(), 0U); }

TEST(SystemUtilTest, SnowflakeKeyUnique) {
    constexpr int kCount = 1000;
    std::set<time_t> keys;
    for (int i = 0; i < kCount; ++i) {
        const time_t key = SystemUtil::generateSnowflakeKey();
        EXPECT_GT(key, 0);
        keys.insert(key);
    }
    EXPECT_EQ(keys.size(), static_cast<size_t>(kCount));
}

// ---------------------------------------------------------------------------
// 新增场景（P7）
// ---------------------------------------------------------------------------

/// [正] Backtrace 返回非空调用栈
/// @attention Q20：项目链接未开 -rdynamic，backtrace_symbols 输出仅
/// 地址/偏移而无函数名，符号断言不适用；开启后可加符号包含性断言
TEST(SystemUtilTest, BacktraceFramesNonEmpty) {
    const auto frames = SystemUtil::Backtrace(64, 1);
    ASSERT_FALSE(frames.empty());
    EXPECT_GE(frames.size(), 2U);  // 跳过 1 帧后仍有调用方

    for (const std::string& frame : frames) {
        EXPECT_FALSE(frame.empty());
    }
}

/// [正] BacktraceToString 前缀逐行输出
TEST(SystemUtilTest, BacktraceToStringPrefix) {
    const std::string text = SystemUtil::BacktraceToString(32, 1, "# ");
    ASSERT_FALSE(text.empty());

    const size_t first_pos = text.find("# ");
    EXPECT_EQ(first_pos, 0U);  // 首行即带前缀
}

/// [并] 雪花 key 并发全唯一（8×1000，加锁 + 12 位序列 + 耗尽等待）
TEST(SystemUtilTest, SnowflakeKeyConcurrentUnique) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    constexpr int kTotal = kThreads * kPerThread;

    std::vector<std::set<time_t>> per_thread(static_cast<size_t>(kThreads));
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &per_thread]() {
            auto& keys = per_thread[static_cast<size_t>(t)];
            for (int i = 0; i < kPerThread; ++i) {
                keys.insert(SystemUtil::generateSnowflakeKey());
            }
        });
    }
    for (auto& thread : workers) {
        thread.join();
    }

    std::set<time_t> merged;
    size_t total = 0;
    for (const auto& keys : per_thread) {
        total += keys.size();
        merged.insert(keys.begin(), keys.end());
    }
    EXPECT_EQ(total, static_cast<size_t>(kTotal));          // 线程内唯一
    EXPECT_EQ(merged.size(), static_cast<size_t>(kTotal));  // 跨线程唯一
}

/// [界] getCoreNum 两次调用一致；getWorkId 契约锁定（当前硬编码 1）
TEST(SystemUtilTest, CoreNumStableAndWorkIdContract) {
    EXPECT_EQ(SystemUtil::getCoreNum(), SystemUtil::getCoreNum());
    EXPECT_EQ(SystemUtil::getWorkId(), 1);  // 契约锁定：机器号未实现前恒为 1
}

}  // namespace test
}  // namespace bus
