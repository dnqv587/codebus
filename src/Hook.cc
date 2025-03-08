#include "Hook.h"
#include <cstdlib>
#include <dlfcn.h>
//#include <iostream>

namespace bus
{
#define HOOK_FUN    \
        XX(sleep)   \
        XX(malloc)

static thread_local bool t_hook_enable = false;
bool Hook::isHookEnable()
{
    return t_hook_enable;
}

void Hook::setHook(bool flag)
{
    t_hook_enable = flag;
}


}

extern "C"
{
#define XX(name) name##_func name##_hook = nullptr;
    HOOK_FUN
#undef XX

unsigned int sleep(unsigned int seconds)
{
    if(!bus::t_hook_enable)
    {
        return sleep_hook(seconds);
    }
    //std::cout<<"sleep"<<std::endl;
    return sleep_hook(seconds);
}

void* malloc(size_t size)
{
    if (!bus::t_hook_enable)
    {
        return malloc_hook(size);
    }
    //std::cout << "malloc:" << size << std::endl;
    return malloc_hook(size);
}
}

struct HookInit
{
    HookInit()
    {
#define XX(name) \
        if(!name##_hook){         \
        name##_hook = (name##_func)::dlsym(RTLD_NEXT,#name);}

        HOOK_FUN
#undef XX
    }
};

static HookInit hook_init;

#undef HOOK_FUN