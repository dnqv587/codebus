//***********************************************
// name      : HttpProtocolTest.cc
// brief     : HttpProtocol 枚举恒等性与字符串转换测试
// attention : None
//***********************************************
#include <gtest/gtest.h>
#include "net/http/HttpProtocol.h"

using namespace bus::http;

/// [正] 枚举数值与 C 枚举恒等（static_cast 零成本互转契约）
TEST(HttpProtocolTest, EnumValuesMatchCEnum) {
    EXPECT_EQ(static_cast<int>(HttpMethod::kDELETE), HTTP_DELETE);
    EXPECT_EQ(static_cast<int>(HttpMethod::kGET), HTTP_GET);
    EXPECT_EQ(static_cast<int>(HttpMethod::kPOST), HTTP_POST);
    EXPECT_EQ(static_cast<int>(HttpStatus::kOK), HTTP_STATUS_OK);
    EXPECT_EQ(static_cast<int>(HttpStatus::kNOT_FOUND), HTTP_STATUS_NOT_FOUND);
    EXPECT_EQ(static_cast<int>(HttpStatus::kINTERNAL_SERVER_ERROR), HTTP_STATUS_INTERNAL_SERVER_ERROR);
}

/// [正] 方法名转换
TEST(HttpProtocolTest, MethodToString) {
    EXPECT_EQ(HttpMethodToString(HttpMethod::kGET), "GET");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kPOST), "POST");
    EXPECT_EQ(HttpMethodToString(HttpMethod::kOPTIONS), "OPTIONS");
    /// [界] 非法值返回 C 库哨兵（ELEM_AT 边界检查，小写）
    EXPECT_EQ(HttpMethodToString(static_cast<HttpMethod>(0xFF)), "<unknown>");
}

/// [正] 状态码短语转换
TEST(HttpProtocolTest, StatusToString) {
    EXPECT_EQ(HttpStatusToString(HttpStatus::kOK), "OK");
    EXPECT_EQ(HttpStatusToString(HttpStatus::kMOVED_PERMANENTLY), "Moved Permanently");
    EXPECT_EQ(HttpStatusToString(HttpStatus::kNOT_FOUND), "Not Found");
    EXPECT_EQ(HttpStatusToString(HttpStatus::kPAYLOAD_TOO_LARGE), "Payload Too Large");
    /// [界] 非法值（switch default 哨兵）
    EXPECT_EQ(HttpStatusToString(static_cast<HttpStatus>(999)), "<unknown>");
}
