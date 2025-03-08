#ifndef NETBUS_HOOK_H
#define NETBUS_HOOK_H

#include <unistd.h>

namespace bus
{
namespace Hook
{
    bool isHookEnable();
    void setHook(bool flag);

}
}

extern "C"
{
typedef unsigned int (*sleep_func)(unsigned int seconds);
typedef void* (*malloc_func)(size_t size);
}

#endif //NETBUS_HOOK_H
