
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
        thread/ThreadPool.cc
        config/config.cc
        time/DateTime.cc
        config/Value.cc
        config/Value_view.cc
        util/Util.cc
        Hook.cc
        net/EventLoop.cc
        net/Channel.cc
        net/EventDispatcher.cc
        net/EventDispatcher.h
        net/Socket.cc
        net/Address.cc
        Timer.cc
        net/TcpServer.cc
)

#链接动态库
target_link_libraries(netbus pthread dl)