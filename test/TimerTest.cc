#include <iostream>
#include <future>
#include "net/EventLoop.h"
#include "Timer.h"
#include "log/Logger.h"
#include "config/config.h"


using namespace uf;
int main()
{
    ConfigManager::getInstance()->Load("root","test.toml");
    LoggerManager::getInstance()->LoadConfig("root");
    EventLoop loop;
    TimerManager man(std::ref(loop));
    auto fu = std::async([&man](){

        int count=0;

        man.addTimer([&count](){
            std::cout<<++count<<std::endl;
        },std::chrono::system_clock::now()+ std::chrono::seconds(5),std::chrono::seconds(1));

    });
    loop.Loop();
    return 0;
}