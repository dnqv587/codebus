#include <config/config.h>
#include <iostream>
#include <string>

static constexpr const std::string_view str=R"(
        str = "hello world"
		integer=8888

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
	{
		ConfigForToml config("test", "test");
		config.LoadFile("test.toml");
		auto fish = config.getConfig("str").asString();
		auto integer = config.getConfig("integer").asInteger();
		auto fish2 = config.getConfig("str").asString();
		std::cout << fish << std::endl;
		std::cout << fish2 << std::endl;
		std::cout << integer << std::endl;
	}
	std::cout<<"end"<<std::endl;

    return 0;
}