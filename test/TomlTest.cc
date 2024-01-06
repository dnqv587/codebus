#include <toml++/toml.hpp>
#include <iostream>
#include <string>

static constexpr const std::string_view str=R"(
        str = "hello world"
        time = 1979-05-27T00:32:00.9999999991Z

        numbers = [ 1, 2, 3, [ "tomato", "onion", "mushroom", "lettuce" ], 5.0 ]
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

    std::cout<<tb.at_path("str").as_string()->get()<<std::endl;
    std::cout<<*tb.at_path("time").as_date_time()<<std::endl;
    //std::cout<<tb.at_path("animals.cats").as_table()<<std::endl;

    std::cout<<tb.at_path("animals").as_table()<<std::endl;
    tb.at_path("numbers").as_array()->for_each([](auto&& val,int i){

        std::cout<<val<<i<<std::endl;
    });
    //std::cout<<toml::json_formatter(tb)<<std::endl;
    return 0;
}