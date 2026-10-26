/// @brief 格式项类生成模板头：由包括方在 bus 命名空间内以 ITEM_NAME/
///        ITEM_BODY 宏驱动多次包含。**本文件不得包 namespace**——包含点
///        已处于 namespace bus 内，再包一层会使生成类落入 bus::bus（46bc2d3
///        曾因此破坏编译，已回退）
#ifdef FORMATITEM
class ITEM_NAME : public LogFormatter::FormatItem
{
 public:
	void Format(std::stringstream& stream,
		LogContext& context,
		std::string&& suffix = "",
		std::string&& TimeFMT = "") override
	{

#undef OUTPUT
#define OUTPUT(X)  stream<<(X)<<suffix

		ITEM_BODY;
	}
};

#undef ITEM_NAME
#undef ITEM_BODY
#undef OUTPUT
#endif
