#pragma once

#include "trafficmcmt/core/Types.h"

namespace tmcmt {

/// UTC milliseconds since the Unix epoch (`system_clock`).
///
/// Used for every timestamp that is shared between nodes or written to disk.
/// The value can jump when the host clock is adjusted (NTP), so it must never
/// be used to measure elapsed time.
[[nodiscard]] Timestamp NowMs() noexcept;

/// Monotonic milliseconds (`steady_clock`).
///
/// Only differences are meaningful; the epoch is unspecified. Used for
/// timeouts, idle detection and latency measurements that must survive a
/// wall-clock step.
[[nodiscard]] Timestamp SteadyNowMs() noexcept;

} // namespace tmcmt
