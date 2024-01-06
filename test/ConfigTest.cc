#include <config/config.h>
#include <iostream>
#include <string>

static constexpr const std::string_view str=R"(
        str = "hello world"
		integer=8888
		date = 1979-05-27T00:32:00.999999+12:00

		name = { first = "汤姆", last = "普雷斯顿—维尔纳" }

        numbers = [ 1, 2, 3,  [ "tomato", "onion", "mushroom", "lettuce" ], 5.0 ]
        vegetables = [ "tomato", "onion", "mushroom", "lettuce" ]
        minerals = [ "quartz", "iron", "copper", "diamond" ]

        [animals]
        cats = [ "tiger", "lion", "puma" ]
        birds = [ "macaw", "pigeon", "canary" ]
        fish = [ "salmon", "trout", "carp" ]



		[[products]]
		name = "锤子"
		sku = 738594937

		[[products]]  # 数组里的空表

		[[products]]
		name = "钉子"
		sku = 284758393

		color = "灰色"
)";

int main()
{
	std::vector<Value::ptr> array;
	{
		ConfigForToml config("test");
		config.Load(str);
		auto fish = config.getConfig("str").asString();
		auto integer = config.getConfig("integer").asInteger();
		auto fish2 = config.getConfig("str").asString();
		std::cout << fish << std::endl;
		std::cout << fish2 << std::endl;
		std::cout << integer << std::endl;
        DateTime::useSpaceSign();

		auto date=config.getConfig("date").asString("date");
		std::cout<<date<<std::endl;

		array=config.getConfig("numbers").asArray();
        for(auto& i:array)
        {
            std::cout<<*i<<std::endl;
        }

        auto table=config.getConfig("animals").asTable();
        for(auto& i:table)
        {
            std::cout<<i.first<<":"<<*i.second<<std::endl;
        }

        auto tt=config.getConfig("str");
        //auto ttt=tt.copy();

		auto arr=config.getConfig("products").asArrayTable();
		for(auto& i:arr)
		{
			for(auto& j:i)
			{
				std::cout<<j.first<<":"<<*j.second<<std::endl;
			}

		}

		auto onearr=config.getConfig("name").asTable();
		for(auto& i:onearr)
		{
			std::cout<<i.first<<","<<i.second->asString()<<std::endl;
		}

	}
	std::cout<<"end"<<std::endl;


    return 0;
}