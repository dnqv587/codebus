#最小版本
cmake_minimum_required (VERSION 3.0)
#工程名
project (NETBUS)
#C++版本
set(CMAKE_CXX_STANDARD  17)
#编译类型
set(CMAKE_BUILD Debug)
#动态库文件输出目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib)
# 静态库文件输出目录
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib)
#可执行文件输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
#头文件地址
include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${PROJECT_SOURCE_DIR}/3rd)
#动态库链接地址
link_directories(${PROJECT_SOURCE_DIR}/lib)
#子目录
add_subdirectory(src)
add_subdirectory(test)

