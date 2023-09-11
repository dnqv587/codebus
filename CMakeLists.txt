#最小版本
cmake_minimum_required (VERSION 3.0)
#工程名
project (NETBUS)
#C++版本
set(CMAKE_CXX_FLAGS "{CMAKE_CXX_FLAGS} -std=c++14")
#编译类型
set(CMAKE_BUILD Debug)
#库文件输出目录
set(LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib)
#可执行文件输出目录
set(EXECUTE_OUT_PATH ${PROJECT_SOURCE_DIR}/bin)

link_directories(${PROJECT_SOURCE_DIR}/lib)
add_subdirectory(src)

