#***********************************************
# name      : test/CMakeLists.txt
# brief     : 单元测试构建配置（一单元一目标，目录镜像 src/ 结构，接入 CTest）
# attention : 测试目标统一链接 gtest_main，测试源文件不写 main；
#             子目录源文件经 ${CMAKE_CURRENT_SOURCE_DIR} include 路径
#             引用根级 TestHelper.h / EventLoopTestHelper.h
#***********************************************

# 注册一个测试可执行目标
# @param TEST_NAME     目标名（= 源文件名去 .cc，全局唯一）
# @param TEST_SOURCE   源文件相对 test/ 的路径
# @param TIMEOUT       可选超时秒数（默认 60）
# @param NO_LEAK_CHECK 可选开关：目标含有意泄漏绕行（现存仅 NetworkStressTest
#                      的 D27 僵尸连接），仅对其关闭 ASAN 泄漏检测，其余目标保持全开
function(codebus_add_test TEST_NAME TEST_SOURCE)
    set(options NO_LEAK_CHECK)
    set(oneValueArgs TIMEOUT)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${TEST_NAME} codebus gtest_main gtest pthread)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
    if(ARG_NO_LEAK_CHECK)
        set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
    endif()
endfunction()

#---------------- 根级源文件（镜像 src/ 根） ------
codebus_add_test(SliceTest           SliceTest.cc)
codebus_add_test(BufferTest          BufferTest.cc)
codebus_add_test(ConfigTest          ConfigTest.cc)
codebus_add_test(TimerTest           TimerTest.cc TIMEOUT 120)

#---------------- base ----------------
codebus_add_test(TypeTest            base/TypeTest.cc)
codebus_add_test(MacroTest           base/MacroTest.cc)
codebus_add_test(ViewTest            base/ViewTest.cc)
codebus_add_test(SingletonTest       base/SingletonTest.cc)
codebus_add_test(BlockingQueueTest   base/BlockingQueueTest.cc)

#---------------- exception ----------
codebus_add_test(ExceptionTest       exception/ExceptionTest.cc)

#---------------- log（自底向上） ------
codebus_add_test(LogFormatterTest    log/LogFormatterTest.cc)
codebus_add_test(LogAppenderTest     log/LogAppenderTest.cc)
codebus_add_test(LoggingTest         log/LoggingTest.cc)
codebus_add_test(LoggerTest          log/LoggerTest.cc)
codebus_add_test(LoggerManagerTest   log/LoggerManagerTest.cc)

#---------------- time ----------------
codebus_add_test(TimestampTest       time/TimestampTest.cc)
codebus_add_test(DateTimeTest        time/DateTimeTest.cc)

#---------------- util ----------------
codebus_add_test(TimeUtilTest        util/TimeUtilTest.cc)
codebus_add_test(StringUtilTest      util/StringUtilTest.cc)
codebus_add_test(SystemUtilTest      util/SystemUtilTest.cc)

#---------------- thread --------------
codebus_add_test(MutexTest           thread/MutexTest.cc)
codebus_add_test(ConditionTest       thread/ConditionTest.cc)
codebus_add_test(SemaphoreTest       thread/SemaphoreTest.cc)
codebus_add_test(CountDownLatchTest  thread/CountDownLatchTest.cc)
codebus_add_test(ThreadTest          thread/ThreadTest.cc)
codebus_add_test(ThreadPoolTest      thread/ThreadPoolTest.cc)
codebus_add_test(ProcessInfoTest     thread/ProcessInfoTest.cc)

#---------------- net ------------------
codebus_add_test(AddressTest         net/AddressTest.cc)
codebus_add_test(SocketTest          net/SocketTest.cc)
codebus_add_test(ServerStatusTest    net/ServerStatusTest.cc)
codebus_add_test(ChannelTest         net/ChannelTest.cc)
codebus_add_test(EventDispatcherTest net/EventDispatcherTest.cc)
codebus_add_test(EventLoopTest       net/EventLoopTest.cc TIMEOUT 120)
codebus_add_test(ListenerTest        net/ListenerTest.cc)
codebus_add_test(TCPConnectionTest   net/TCPConnectionTest.cc)

#---------------- integration ----------
codebus_add_test(NetworkStressTest   integration/NetworkStressTest.cc TIMEOUT 300 NO_LEAK_CHECK)
