#pragma once
#include <functional>
#include <string_view>
#include <string>
#include "base/noncopyable.hpp"
#include "net/Address.h"

namespace uf{

class EventLoop;
class TcpServer:noncopyable
{
public:
    TcpServer(std::reference_wrapper<EventLoop> loop,std::string_view name ,const Address& addr);

private:
    class Acceptor

    std::reference_wrapper<EventLoop> m_loop;
    std::string m_name;
};


}