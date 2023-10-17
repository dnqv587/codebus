#include <iostream>
#include <log/Logger.h>
#include <unistd.h>

LogAppender* SyncAppender=new FileLogAppender("SyncAppender");


LogAppender* AsyncAppender=new FileLogAppender("AsyncAppender",AppenderAction::ASYNC,1,1024,10*FileSize::MB);

LogAppender* logout=new StdoutLogAppender();

//LogAppender* logerr=new StderrLogAppender();

int main()
{

    int i=0;
    while(i<65535)
    {
        std::shared_ptr<std::stringstream> ss = std::make_shared<std::stringstream>("aqewqcasacsad德农请我哦对你抢我电脑青豌豆你请我对哦IQ我你起期待你起哦对去hbafiosuc haiuowdbnciqwducioq4829120640186的缺点去外地请问带我去749861 8q7g4 r789q0wef4q8674 6rwe8g 489er6g4---"+std::to_string(i)+'\n');

        //SyncAppender->append(ss);
        AsyncAppender->append(ss);
        logout->append(ss);
        //logerr->append(ss);
        //usleep(10);
        ++i;
    }
    return 0;
}