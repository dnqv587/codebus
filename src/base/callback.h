#pragma once
#include <functional>
#include "time/Timestamp.h"

namespace uf{

using EventCallback = std::function<void ()>;
using ReadEventCallback =  std::function<void ()>;

}