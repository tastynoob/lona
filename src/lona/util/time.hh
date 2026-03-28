#pragma once

#include <chrono>

namespace lona {

using Clock = std::chrono::steady_clock;

template<typename TimePoint>
double
elapsedMillis(TimePoint start, TimePoint end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace lona
