#生成可执行文件
add_executable(logFormatterTest logFormatterTest.cc )
add_executable(LogAppenderTest LogAppenderTest.cc)
add_executable(ConditionTest ConditionTest.cc)
add_executable(ThreadTest ThreadTest.cc)
add_executable(AsynLogAppendTest AsynLogAppendTest.cc)
add_executable(LoggerTest LoggerTest.cc)

#链接动态库
target_link_libraries(logFormatterTest netbus)
target_link_libraries(LogAppenderTest netbus)
target_link_libraries(ConditionTest netbus)
target_link_libraries(ThreadTest netbus)
target_link_libraries(AsynLogAppendTest netbus)
target_link_libraries(LoggerTest netbus)