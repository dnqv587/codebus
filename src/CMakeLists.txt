
#生成动态库
add_library(netbus SHARED
        thread/CurrentThread.cc
        thread/ProcessInfo.cc
        time/Timestamp.cc
        log/Logger.cc
        log/Logging.cc
        log/LogFormatter.cc
        log/LogAppender.cc
        thread/Thread.cc
        thread/Condition.hpp
        thread/Semaphore.hpp
        thread/ThreadPool.cc
)

#链接动态库
target_link_libraries(netbus pthread)