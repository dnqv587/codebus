#include <toml++/toml.hpp>
#include <iostream>
#include <string>

static constexpr const std::string_view str=R"(
        str = "hello world"
        time = 2019-10-12 14:20:50.52+12:00

        numbers = [ 1, 2, 3, "four", 5.0 ]
        vegetables = [ "tomato", "onion", "mushroom", "lettuce" ]
        minerals = [ "quartz", "iron", "copper", "diamond" ]

        [animals]
        cats = [ "tiger", "lion", "puma" ]
        birds = [ "macaw", "pigeon", "canary" ]
        fish = [ "salmon", "trout", "carp" ]
)";

int main()
{
    toml::table tb=toml::parse(str);

    //std::cout<<tb.at_path("str").as_integer()->get()<<std::endl;
    std::cout<<*tb.at_path("time").as_date_time()<<std::endl;

    tb["animals"]["cats"].as_array()->for_each([](auto&& val){

        //std::cout<<val<<std::endl;
    });
    //std::cout<<toml::json_formatter(tb)<<std::endl;
    return 0;
}