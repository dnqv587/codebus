//***********************************************
// name      : ViewTest.cc
// brief     : base 模块单元测试：被测 src/base/view.h（借用视图，不拥有生命周期）
// attention : View 不拥有对象，使用时须保证被引用对象存活（RAII 契约的
//             文档化部分不做用例，见 CLAUDE.md 所有权约定）
//***********************************************
#include <gtest/gtest.h>
#include "base/view.h"

#include <memory>
#include <string>
#include <utility>

namespace bus
{
namespace test
{

namespace
{

struct Point
{
    int x{0};
    int y{0};
};

} // namespace

/// [正] reference_wrapper 隐式构造，Peer/NativeHandle 指向原对象
TEST(ViewTest, PeerAndNativeHandle)
{
    Point point{1, 2};
    View<Point> view(std::ref(point));

    EXPECT_EQ(&view.Peer(), &point);
    EXPECT_EQ(view.NativeHandle(), &point);
    EXPECT_EQ(view.Peer().x, 1);
    EXPECT_EQ(view.Peer().y, 2);
}

/// [正] 经视图修改即修改原对象（借用语义）
TEST(ViewTest, MutationVisibleThroughView)
{
    Point point{0, 0};
    View<Point> view(std::ref(point));

    view.Peer().x = 10;
    view.NativeHandle()->y = 20;

    EXPECT_EQ(point.x, 10);
    EXPECT_EQ(point.y, 20);
}

/// [界] 拷贝与移动后的视图仍指向同一对象
TEST(ViewTest, CopyAndMoveStillViewSameObject)
{
    Point point{7, 8};
    View<Point> original(std::ref(point));

    View<Point> copy = original;
    EXPECT_EQ(copy.NativeHandle(), &point);

    View<Point> moved = std::move(original);
    EXPECT_EQ(moved.NativeHandle(), &point);
    EXPECT_EQ(&moved.Peer(), &point);
}

/// [正] 对智能指针管理对象建视图（常见用法：Channel/Listener 对 loop 的引用）
TEST(ViewTest, ViewOverSmartPointee)
{
    auto managed = std::make_unique<std::string>("payload");
    const std::string* raw = managed.get();

    View<std::string> view(std::ref(*managed));
    EXPECT_EQ(view.NativeHandle(), raw);
    EXPECT_EQ(view.Peer(), "payload");
}

} // namespace test
} // namespace bus
