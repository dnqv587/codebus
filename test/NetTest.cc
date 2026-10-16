#include <iostream>
#include "net/EventLoop.h"
#include "net/Listener.h"
#include "net/Address.h"
#include "log/Logger.h"
#include "Buffer.hpp"
#include <utility>

using namespace bus;
int main(){
    // LoggerManager::getInstance()->LoadConfig("test");
    
    // EventLoop loop;
    // Address addr("127.0.0.1",8899);
    // Listener listen(std::ref(loop),addr);
    // listen.Listen();
    // listen.setNewConnectionCallback([](Socket fd,Address addr){
    //     std::cout<<"new connection:"<<addr.ipString()<<":"<<addr.port()<<std::endl;
    // });
    // loop.Loop();

    std::vector<std::byte> s {std::byte('0'),std::byte('1'),std::byte('2'),std::byte('3')};
    std::vector<std::byte> s1 {std::byte('1'),std::byte('1'),std::byte('2'),std::byte('3')};
    typename bus::Slice ss (s.begin(),4);
    std::cout<<ss.toString()<<std::endl;

    std::cout<<std::boolalpha<<(s == s1)<<std::endl;
    std::cout<<std::boolalpha<<(s > s1)<<std::endl;
    std::cout<<std::boolalpha<<(s < s1)<<std::endl;
    std::cout<<std::boolalpha<<(s != s1)<<std::endl;

    // std::vector<std::byte> s2 (ss.begin(),ss.size());
    // std::cout<<ss.data()<<std::endl;
    std::vector<std::byte> s2 {std::byte('0'),std::byte('1'),std::byte('2'),std::byte('3'),std::byte('\n'),std::byte('4')};
    typename bus::Slice ss1 (s2.begin(),s2.end());
    Buffer buf{5};
    buf.Write(ss);
    buf.Write(ss1);
    Slice c1  = buf.Read(1);
    std::cout<<'['<<c1<<']'<<std::endl;
    Slice c2  = buf.Read(1);
    std::cout<<'['<<c2<<']'<<std::endl;
    Slice c3  = buf.Read(1);
    std::cout<<'['<<c3<<']'<<std::endl;
    Slice c4  = buf.Peek(1);
    std::cout<<'['<<c4<<']'<<std::endl;
    Slice c5  = buf.Read(1);
    std::cout<<'['<<c5<<']'<<std::endl;
    Slice c6  = buf.PeekAll();
    std::cout<<'['<<c6<<']'<<std::endl;

    auto eol_end = buf.FindEOL();
    auto s_begin = buf.begin();
    std::vector<std::byte> eol_str(s_begin,eol_end);
    typename bus::Slice ss2 (eol_str.begin(),eol_str.end());
    std::cout<<ss2<<std::endl;
}