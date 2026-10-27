#include "Util.h"
#include <cstdint>
#include <execinfo.h>
#include <mutex>
#include <unistd.h>
#include "type.h"
#include "log/Logger.h"

namespace bus {
static Logger* g_logger = GET_LOGGER("system");

/// @brief 进程启动时间
static Timestamp ProcessRunTime = Timestamp::now();

Timestamp TimeUtil::getElapse() {
    return Timestamp::now() - ProcessRunTime;
}

off64_t StringUtil::StrToFileSize(const char* str) {
    off64_t size = 0;
    while (str) {
        if (*str == '\0') {
            return size;
        }
        if (*str >= '0' && *str <= '9') {
            size = size * 10 + (*str - '0');
        } else {
            if (*(str + 1) != 'B') {
                return size;
            }
            switch (str[0]) {
                case 'K':
                    return operator""_KB(size);
                    break;
                case 'M':
                    return operator""_MB(size);
                    break;
                case 'G':
                    return operator""_GB(size);
                    break;
                case 'T':
                    return operator""_TB(size);
                    break;
                default:
                    return size;
            }
        }
        ++str;
    }

    return size;
}

std::vector<std::string> SystemUtil::Backtrace(int size, int skip) {
    std::vector<std::string> bt;
    void** buffer = (void**)malloc(sizeof(void*) * size);
    int s = ::backtrace(buffer, size);
    bt.reserve(s - skip);

    char** strings = ::backtrace_symbols(buffer, s);
    for (int i = skip; i < s; ++i) {
        bt.emplace_back(strings[i]);
    }
    free(buffer);
    free(strings);

    return bt;
}

std::string SystemUtil::BacktraceToString(int size, int skip, std::string_view prefix) {
    auto bt = Backtrace(size, skip);
    std::stringstream ss;
    for (const auto& i : bt) {
        ss << prefix << i << std::endl;
    }
    return ss.str();
}

void SystemUtil::Abort() {
    LOG_FATAL(g_logger) << "the system will exit soon, Backtrace: " << BacktraceToString(64, 3);
    abort();
}

time_t SystemUtil::generateSnowflakeKey() {
    static std::mutex mtx;
    static time_t lastMS = 0;
    static uint16_t sequence = 0;
    constexpr uint16_t kSequenceMask = 0xFFF;  // key 低 12 位为序列

    std::lock_guard<std::mutex> lock(mtx);
    time_t currentMS = Timestamp::now().getMilliSecond();
    if (lastMS > currentMS) {
        throw std::logic_error("Occurrence time callback!");
    }

    if (currentMS > lastMS) {
        sequence = 0;  // 新毫秒序列归零
    } else {
        sequence = (sequence + 1) & kSequenceMask;
        if (sequence == 0) {  // 本毫秒 4096 序列耗尽：等待下一毫秒
            do {
                currentMS = Timestamp::now().getMilliSecond();
            } while (currentMS == lastMS);
            sequence = 0;
        }
    }

    lastMS = currentMS;
    return (currentMS - TimeUtil::getEpoch().getMilliSecond()) << 22 | getWorkId() << 12 | sequence;
}

uint32_t SystemUtil::getCoreNum() {
    static const uint32_t core_num = static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
    return core_num;
}

}  // namespace bus