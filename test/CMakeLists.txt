#生成动态库
add_executable(logFormatterTest logFormatterTest.cc )

#链接动态库
target_link_libraries(logFormatterTest netbus)