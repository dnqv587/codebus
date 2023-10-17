#include <iostream>
#include <log/LogAppender.h>
#include <unistd.h>
AsynAppendFile logAppender("Asynlog",40960,3,1024);

int main()
{
    logAppender.start();
    int i=0;
    while(1)
    {
        std::shared_ptr<std::stringstream> ss = std::make_shared<std::stringstream>("aqewqcasacsad德农请我哦对你抢我电脑青豌豆你请我对哦IQ我你起期待你起哦对去hbafiosuc haiuowdbnciqwducioq4829120640186的缺点去外地请问带我去749861 8q7g4 r789q0wef4q8674 6rwe8g 489er6g4---"+std::to_string(i)+'\n');
        std::cout<<ss->str()<<std::endl;

        logAppender.Append(std::move(ss));
        usleep(100);
        ++i;
    }
    return 0;
}