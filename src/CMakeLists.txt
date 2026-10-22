
#生成动态库
add_library(codebus SHARED
        thread/CurrentThread.cc
        thread/ProcessInfo.cc
        time/Timestamp.cc
        log/Logger.cc
        log/Logging.cc
        log/LogFormatter.cc
        log/LogAppender.cc
        thread/Thread.cc
        thread/ThreadPool.cc
        Config.cc
        time/DateTime.cc
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
        net/Listener.cc
        net/TCPConnection.cc
        net/EventLoopThread.cc
)

#链接动态库
target_link_libraries(codebus pthread tomlc17)