#pragma once
#include <functional>
#include "Timestamp.h"

namespace uf {

using EventCallback = std::function<void()>;
using ReadEventCallback = std::function<void()>;

}  // namespace uf