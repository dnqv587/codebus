#pragma once
#include <type_traits>
#include <atomic>
#include <string>
#include "base/Macro.h"

template <bool isAtomic = true>
class ServerStatus
{
public:
    enum Status{
        kNull           = 0,
        kInitializing   = 1,
        kInitialized    = 2,
        kStarting       = 3,
        kRunning        = 4,
        kStopping       = 5,
        kStopped        = 6
    };

    ATTR_NODISCARD
    std::string StatusToString() const noexcept
    {
        switch(m_status)
        {
            case kNull:
                return "kNull";
            case kInitializing:
                return "kInitializing";
            case kInitialized:
                return "kInitialized";
            case kStarting:
                return "kStarting";
            case kRunning:
                return "kRunning";
            case kStopping:
                return "kStopping";
            case kStopped:
                return "kStopped";
        }
    }

    ATTR_PURE_INLINE
    bool isRunning() const noexcept
    {return m_status == kRunning;}
    ATTR_PURE_INLINE
    bool isStopping() const noexcept
    {return m_status == kStopping;}
    ATTR_PURE_INLINE
    bool isStopped() const noexcept
    {return m_status == kStopped;}

protected:
    std::conditional_t<isAtomic,std::atomic<Status>,Status> m_status = {kNull};
};