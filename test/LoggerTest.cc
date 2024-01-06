#include <log/Logger.h>
#include <config/config.h>
#include <unistd.h>
#include <iostream>
#include <exception/Exception.hpp>

Logger logger("test");
MutexLock mutex;

//std::atomic<int> _i;
int _i;

void func()
{
    while(_i<65535)
    {
		std::cout<<++_i<<std::endl;
		//MutexLockGuard lock(mutex);
        //sleep(1);
		//LOG_WARN(logger) << "aqewqcasacsad德农请我哦对你抢我电脑青豌豆你请我对哦IQ我你起期待你起哦对去hbafiosuc haiuowdbnciqwducioq4829120640186的缺点去外地请问带我去749861 8q7g4 r789q0wef4q86---" <<"--:"<<++_i;
    }
}


void test( std::function<const char* ()> str)
{
    std::cout<<GET_CONSTEVAL(str)<<std::endl;
    //const char* ss=StringUtil::BaseFileName(GET_CONSTEVAL(str));
    //std::cout<<ss<<std::endl;
    //str +=2;
    //return GET_CONSTEVAL(str);
}

class Foo
{
public:
    template<typename T>
    constexpr explicit Foo(T file)
    :name(StringUtil::BaseFileName(GET_CONSTEVAL(file)))
    {
		std::cout<<name<<std::endl;
    }
    friend std::ostream& operator<<(std::ostream& lhs , Foo foo)
    {
        lhs<<foo.name;
        return lhs;
    }

    constexpr const char* getName() const
    {
        return this->name;
    }
private:
    const char* name;
};

template<typename T>
const char* print(T ff)
{
    //printf("%s", ff.getName());
	const char* str=StringUtil::BaseFileName(GET_CONSTEVAL(ff));
	//return str;

	return str;
}

int main()
{
    //static_assert(StringUtil::BaseFileName(__FILE__));

	/*
    const char* str=print(CONSTEVAL(__FILE__));
	std::cout<<str<<std::endl;


    //constexpr const char* str=StringUtil::BaseFileName(__FILE__);
   // const char* (*ptr)()=CONSTEVAL(__FILE__);
    std::function<const char* ()> ptr=CONSTEVAL(__FILE__);
    test(CONSTEVAL(__FILE__));
    //std::cout<<ss<<std::endl;
    auto val= CONSTEVAL(StringUtil::BaseFileName(__FILE__));
    const char* sss= GET_CONSTEVAL(val);
    std::cout<<sss<<std::endl;*/

    //constexpr auto i=Logging(logger,StringUtil::BaseFileName(__FILE__),__LINE__,__FUNCTION__,LogLevel::ERROR);
    _i=0;
/*
    logger.setFormatter("%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");
    logger.setLevel(LogLevel::TRACE);

    LogAppender::ptr SyncAppender=LogAppender::ptr(new FileLogAppender("SyncAppender","log",AppenderAction::SYNC,3,1024,4_MB));
    //LogAppender::ptr AsyncAppender=LogAppender::ptr(new FileLogAppender("AsyncAppender",AppenderAction::ASYNC,1,1024,10*FileSize::MB));
    LogAppender::ptr logout=LogAppender::ptr(new StdoutLogAppender());
    logger.addAppender(SyncAppender);
    //logger.addAppender(AsyncAppender);
    logger.addAppender(logout);*/

    //Thread thread(func,"testFunc");
    //thread.run();

//    LOG_ERROR(logger)<<"这里是颜色测试";
//    LOG_INFO(logger)<<"这里是颜色测试";
//    LOG_DEBUG(logger)<<"这里是颜色测试";
//    LOG_FATAL(logger)<<"这里是颜色测试";
//    LOG_TRACE(logger)<<"这里是颜色测试";
//    LOG_WARN(logger)<<"这里是颜色测试";
    ConfigManager::getInstance()->Load("test","test.toml");
	LoggerManager::getInstance()->LoadConfig("test");
	try {
		LOG_THROW(Exception::LoggerError("这是测试 log"));
	}
	catch (...)
	{
		std::cout<<"test"<<std::endl;
	}

    while(_i<65535)
    {
		//logger.trace("测试日志输出");
		//++_i;
		//MutexLockGuard lock(mutex);
        //sleep(1);
        LOG_INFO(LOG_ROOT) <<  "这里是测试logger的性能 1234567890 这里是测试logger的性能 abcdefghijklmnopqrstuvwxyz 这里是测试logger的性能 ABCDEFGHIJKLMNOPQRSTUVWXYZ  中文输入性能测试 " << "---" << ++_i;
        LOG_FMT_ERROR(GET_LOGGER("test"),"%s%s%d","这里是测试logger的性能 1234567890 这里是测试logger的性能 abcdefghijklmnopqrstuvwxyz 这里是测试logger的性能 ABCDEFGHIJKLMNOPQRSTUVWXYZ  中文输入性能测试 ","---",++_i);

    }
    //thread.join();
    return 0;
}