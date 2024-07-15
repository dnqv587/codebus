#include <log/Logger.h>
#include <config/config.h>
#include <iostream>


//std::atomic<int> _i;
//int _i;

void func()
{
    int _i=0;
    while(_i<65535)
    {
        LOG_INFO(LOG_ROOT) <<  "这里是测试logger的性能 1234567890 这里是测试logger的性能 abcdefghijklmnopqrstuvwxyz 这里是测试logger的性能 ABCDEFGHIJKLMNOPQRSTUVWXYZ  中文输入性能测试 " << "---" << ++_i;
        LOG_FMT_ERROR(GET_LOGGER("test"),"%s%s%d","这里是测试logger的性能 1234567890 这里是测试logger的性能 abcdefghijklmnopqrstuvwxyz 这里是测试logger的性能 ABCDEFGHIJKLMNOPQRSTUVWXYZ  中文输入性能测试 ","---",_i);
    }
}


int main()
{

	//Logger logger("root");
    //logger.setFormatter("%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");
    //logger.setLevel(LogLevel::TRACE);

    //LogAppender::ptr SyncAppender=LogAppender::ptr(new FileLogAppender("SyncAppender","log",AppenderAction::SYNC,3,1024,4_MB));
    //LogAppender::ptr AsyncAppender=LogAppender::ptr(new FileLogAppender("AsyncAppender",AppenderAction::ASYNC,1,1024,10*FileSize::MB));
    //LogAppender::ptr logout=LogAppender::ptr(new StdoutLogAppender());
    //logger.addAppender(SyncAppender);
    //logger.addAppender(AsyncAppender);
    //logger.addAppender(logout);

    ConfigManager::getInstance()->Load("test","test.toml");
	LoggerManager::getInstance()->LoadConfig("test");

//    LOG_ERROR(LOG_ROOT)<<"这里是颜色测试";
//    LOG_INFO(LOG_ROOT)<<"这里是颜色测试";
//    LOG_DEBUG(LOG_ROOT)<<"这里是颜色测试";
//    LOG_FATAL(LOG_ROOT)<<"这里是颜色测试";
//    LOG_TRACE(LOG_ROOT)<<"这里是颜色测试";
//    LOG_WARN(LOG_ROOT)<<"这里是颜色测试";
    //Thread thread(func,"testFunc");
    //thread.run();
    int _i=0;
    while(_i<65535)
    {
		//logger.trace("测试日志输出");
		//++_i;
		//MutexLockGuard lock(mutex);
        //sleep(1);
		++_i;
        LOG_INFO(LOG_ROOT) <<  "这里是测试logger的性能 1234567890 这里是测试logger的性能 abcdefghijklmnopqrstuvwxyz 这里是测试logger的性能 ABCDEFGHIJKLMNOPQRSTUVWXYZ  中文输入性能测试 " << "---" << _i;
        LOG_FMT_ERROR(GET_LOGGER("test"),"%s%s%d","这里是测试logger的性能 1234567890 这里是测试logger的性能 abcdefghijklmnopqrstuvwxyz 这里是测试logger的性能 ABCDEFGHIJKLMNOPQRSTUVWXYZ  中文输入性能测试 ","---",_i);

    }
    //thread.join();
    return 0;
}