
/// @brief
#ifdef FORMATITEM
class ITEM_NAME:public LogFormatter::FormatItem
{
public:
    void Format(std::stringstream& stream,LogContext& context,std::string&& suffix="",std::string&& TimeFMT="") override
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