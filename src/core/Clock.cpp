#include "trafficmcmt/core/Clock.h"

#include <chrono>

namespace tmcmt {

Timestamp NowMs() noexcept {
    return std::chrono::duration_cast<Timestamp>(std::chrono::system_clock::now().time_since_epoch());
}

Timestamp SteadyNowMs() noexcept {
    return std::chrono::duration_cast<Timestamp>(std::chrono::steady_clock::now().time_since_epoch());
}

} // namespace tmcmt
