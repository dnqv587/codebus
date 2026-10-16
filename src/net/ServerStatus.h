#pragma once
#include <type_traits>
#include <atomic>
#include <string_view>
#include "base/Macro.h"

template <bool isAtomic = true>
class ServerStatus
{
public:
    enum class Status{
        Null           = 0,
        Initializing   = 1,
        Initialized    = 2,
        Running        = 3,
        Stopping       = 4,
        Stopped        = 5,
        Destroy        = 6
    };

    ServerStatus():m_status(Status::Null){}

    ~ServerStatus(){
        m_status = Status::Destroy;
    }

    ATTR_NODISCARD
    std::string_view StatusToString() const noexcept
    {
        switch(m_status)
        {
            case Status::Null:
                return "Null";
            case Status::Initializing:
                return "Initializing";
            case Status::Initialized:
                return "Initialized";
            case Status::Running:
                return "Running";
            case Status::Stopping:
                return "Stopping";
            case Status::Stopped:
                return "Stopped";
            case Status::Destroy:
                return "Destroy";
        }
    }

    Status getStatus() const noexcept{
        return m_status;
    }

    Status setStatus(Status status) noexcept{
        m_status = status;
    }

protected:
    //std::conditional_t<isAtomic,std::atomic<Status>,Status> m_status = {Status::Null};
    Status m_status = Status::Null;
};