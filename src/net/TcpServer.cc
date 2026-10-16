#include "TcpServer.h"
using namespace bus;

TcpServer::TcpServer(EventLoop* loop,std::string_view addr,uint32_t thread_num)
:m_loop(loop),
m_listen_addr(addr)
{

}