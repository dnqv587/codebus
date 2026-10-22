#最小版本
cmake_minimum_required (VERSION 3.10)
#工程名
project (CODEBUS C CXX)
#C++版本
set(CMAKE_CXX_STANDARD  20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#编译选项
#add_compile_options(-Wall -fsanitize=address -fsanitize=leak -fsanitize=undefined -fsanitize=memory)
#编译类型
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
#可选的 Sanitizer 构建（thread/address/undefined），用于并发与内存错误检测
set(CODEBUS_SANITIZER "" CACHE STRING "Enable sanitizer: thread|address|undefined")
if (CODEBUS_SANITIZER)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=${CODEBUS_SANITIZER} -fno-omit-frame-pointer -g")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=${CODEBUS_SANITIZER}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=${CODEBUS_SANITIZER}")
endif()
#启用 CTest
enable_testing()
#动态库文件输出目录
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib)
# 静态库文件输出目录
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/lib)
#可执行文件输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
#头文件地址
include_directories(${PROJECT_SOURCE_DIR}/src 
                    ${PROJECT_SOURCE_DIR}/3rd 
                    ${PROJECT_SOURCE_DIR}/3rd/gtest/include)
#动态库链接地址
link_directories(${PROJECT_SOURCE_DIR}/lib
                 ${PROJECT_SOURCE_DIR}/3rd/gtest/lib/)
#子目录
add_subdirectory(3rd)
add_subdirectory(src)
add_subdirectory(test)