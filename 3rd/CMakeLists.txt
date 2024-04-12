
#生成动态库
add_library(libuv STATIC

)

#链接动态库
target_link_libraries(libuv pthread)