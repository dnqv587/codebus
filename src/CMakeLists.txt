
#生成动态库
add_library(netbus
        thread/CurrentThread.cc)
#链接动态库
target_link_libraries(netbus pthread)