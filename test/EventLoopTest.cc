#include <iostream>
#include <future>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "net/EventLoop.h"
#include "net/Channel.h"
#include "log/Logger.h"
#include "config/config.h"
#include "arpa/inet.h"

using namespace bus;

int main()
{
	ConfigManager::getInstance()->Load("root","test.toml");
	LoggerManager::getInstance()->LoadConfig("root");

	int fd = ::socket(AF_INET,SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,0);

	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in serv{};
	serv.sin_family=AF_INET;
	serv.sin_port=htons(8888);
	serv.sin_addr.s_addr=htonl(INADDR_ANY);
	int r = bind(fd,(struct sockaddr *)&serv,sizeof(serv));
	listen(fd,128);

	char buf[64]{ 0 };

	sockaddr_in6 addr6{};
	socklen_t len = sizeof(sockaddr_in6);

	getsockname(fd,(sockaddr*)&addr6,&len);
	//getpeername(fd,(sockaddr*)&addr6,&len);

	auto* ad= (sockaddr_in*)&addr6;
	char str[64];
	inet_ntop(AF_INET, &ad->sin_addr.s_addr,str,sizeof(str));
	std::cout<<ntohs(ad->sin_port)<<" "<<str<<std::endl;

	EventLoop loop;
	Channel channel(std::ref(loop),fd);
	channel.setReadCallback([fd,&loop, &buf]()
	{
	  struct sockaddr_in client{};
	  socklen_t len = sizeof(client);
	  int cfd = accept(fd, (struct sockaddr*)&client, &len);
	  auto* chan = new Channel(std::ref(loop), cfd);

	  sockaddr_in6 addr6{};
	  socklen_t llen = sizeof(sockaddr_in6);
	  std::cout<<llen<<std::endl;

	  getpeername(cfd,(sockaddr*)&addr6,&llen);

	  auto* ad= (sockaddr_in*)&addr6;
	  char str[64];
	  inet_ntop(AF_INET, &ad->sin_addr.s_addr,str,sizeof(str));
	  std::cout<<ntohs(ad->sin_port)<<" "<<str<< " "<< llen <<std::endl;

	  chan->setReadCallback([cfd,&buf]()
	  {
		bzero(buf,sizeof buf);
		int n = ::read(cfd, buf, sizeof buf);
		std::cout << "read n = " << n << " buf = " << buf ;
	  });
	  chan->setWriteCallback([cfd,&buf]()
	  {
		int n = ::write(cfd,buf,strlen(buf));
		std::cout << "write n = " << n << " buf = " << buf ;
	  });
        chan->EnableReadEvent();
	  //chan->EnableWriteEvent();
	  loop.UpdateChannel(chan);
	});
    channel.EnableReadEvent();
	loop.UpdateChannel(&channel);

	loop.Loop();
	return 0;
}