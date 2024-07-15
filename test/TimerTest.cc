#include <iostream>
#include <future>
#include "net/EventLoop.h"
#include "Timer.h"
#include "log/Logger.h"
#include "config/config.h"

using namespace std::literals;

using namespace uf;
int main()
{
    ConfigManager::getInstance()->Load("root","test.toml");
    LoggerManager::getInstance()->LoadConfig("root");
    EventLoop loop;
    TimerManager man(std::ref(loop));
    std::atomic<int> count=0;

    auto now1 = std::chrono::steady_clock::now();
    auto f1 = std::async([&man,&count,&now1](){
        auto timer = man.addTimer([&count,&now1](){
            auto t=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-now1).count();
            now1=std::chrono::steady_clock::now();
            std::cout<<"f1:"<<++count<<" time="<< t<<std::endl;
        },std::chrono::steady_clock::now()+ 1s,1s);
		return timer;
    });

    auto now2 = std::chrono::steady_clock::now();
	auto f2 = std::async([&man,&count,&now2](){
	  auto timer = man.addTimer([&count,&now2](){
          auto t=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-now2).count();
          now2=std::chrono::steady_clock::now();
          std::cout<<"f2:"<<++count<<" time="<< t<<std::endl;
	  },std::chrono::steady_clock::now()+ 2s,2s);
	  return timer;
	});

    auto now3 = std::chrono::steady_clock::now();
	auto f3 = std::async([&man,&count,&now3](){
	  auto timer = man.addTimer([&count,&now3](){
          auto t=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-now3).count();
          now3=std::chrono::steady_clock::now();
          std::cout<<"f3:"<<++count<<" time="<< t<<std::endl;;
	  },std::chrono::steady_clock::now()+ 3s,3s);
	  return timer;
	});

    auto now4 = std::chrono::steady_clock::now();
	auto f4 = std::async([&man,&count,&now4](){
	  auto timer = man.addTimer([&count,&now4](){
          auto t=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-now4).count();
          now4=std::chrono::steady_clock::now();
          std::cout<<"f4:"<<++count<<" time="<< t<<std::endl;
	  },std::chrono::steady_clock::now()+ 4s,4s);
	  return timer;
	});

    auto now5 = std::chrono::steady_clock::now();
	auto f5 = std::async([&man,&count,&now5](){
	  auto timer = man.addTimer([&count,&now5](){

          auto t=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-now5).count();
          now5=std::chrono::steady_clock::now();
          std::cout<<"f5:"<<++count<<" time="<< t<<std::endl;
	  },std::chrono::steady_clock::now()+ 5s,std::chrono::seconds(5));
	  return timer;
	});


	std::vector<Timer::ptr> timers;
	timers.push_back(f1.get());
	timers.push_back(f2.get());
	timers.push_back(f3.get());
	timers.push_back(f4.get());
	timers.push_back(f5.get());

	auto ff = std::async([timers = std::move(timers)](){

	  std::this_thread::sleep_for(60s);
	  for(auto& i :timers)
	  {
		  i->cancel();
	  }
	});


    //std::this_thread::sleep_for(20s);
    loop.Loop();

    return 0;
}