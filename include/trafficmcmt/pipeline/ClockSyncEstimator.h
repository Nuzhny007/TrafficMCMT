#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::pipeline {

struct ClockSyncOptions {
    /// Samples kept per camera for the median and the regression.
    std::size_t window_samples = 120;
    /// Samples required before `synced()` becomes true.
    std::size_t min_samples = 5;
    /// Hard bound for the estimated offset.
    std::int64_t max_offset_ms = 10'000;
    /// Hard bound for the estimated drift.
    double max_drift_ppm = 1'000.0;
    /// Expected chunking + network delay, subtracted from the raw offset.
    std::int64_t assumed_transport_ms = 250;
    /// MAD multiplier used to reject delay spikes. `<= 0` disables rejection.
    double outlier_sigma = 3.0;
};

/// Linear edge -> master mapping.
///
/// `master = edge + offset_ms + drift_ppm * 1e-6 * (edge - reference)`
///
/// The mapping is invertible, so raw timestamps can be recovered exactly from
/// the corrected tracklet and the correction itself.
struct ClockCorrection {
    Timestamp reference{};
    double offset_ms = 0.0;
    double drift_ppm = 0.0;

    [[nodiscard]] Timestamp Apply(Timestamp edge) const noexcept;
    [[nodiscard]] Timestamp Revert(Timestamp master) const noexcept;
};

struct ClockSyncStats {
    std::size_t samples = 0;
    std::size_t accepted = 0;
    double offset_ms = 0.0;
    double drift_ppm = 0.0;
    /// Median absolute residual of the accepted samples, milliseconds.
    double residual_ms = 0.0;
    bool synced = false;
};

/// Estimates the offset and drift of one camera clock from
/// `(edge timestamp, master receive time)` pairs.
///
/// The offset is a robust median, the drift a Theil-Sen slope; delay spikes are
/// rejected with a MAD filter. The estimator never throws and stays usable
/// before `min_samples` is reached (offset 0, `synced() == false`).
class ClockSyncEstimator {
public:
    explicit ClockSyncEstimator(ClockSyncOptions options = {});

    /// Adds one observation. `master_ms` is the wall-clock time at which the
    /// message was received by the Master.
    [[nodiscard]] Status AddSample(Timestamp edge_ms, Timestamp master_ms);

    [[nodiscard]] bool synced() const noexcept {
        return m_synced;
    }

    /// Correction valid for an edge timestamp close to `edge_now`.
    [[nodiscard]] ClockCorrection Correction(Timestamp edge_now) const noexcept;

    [[nodiscard]] ClockSyncStats Stats() const noexcept;

    void Reset();

private:
    struct Sample {
        Timestamp edge{};
        double delta_ms = 0.0;
    };

    void Recompute();

    ClockSyncOptions m_options;
    std::deque<Sample> m_samples;
    ClockCorrection m_correction;
    ClockSyncStats m_stats;
    bool m_synced = false;
};

/// A tracklet in both time domains: `tracklet` uses Master time, while
/// `raw_begin` / `raw_end` keep the original edge timestamps.
struct SyncedTracklet {
    Tracklet tracklet;
    Timestamp raw_begin{};
    Timestamp raw_end{};
    ClockCorrection correction;
    Timestamp received_at{};
};

/// Applies `correction` to `tracklet.begin`, `tracklet.end` and every
/// observation timestamp, and records the raw values.
[[nodiscard]] SyncedTracklet Synchronize(Tracklet tracklet, const ClockCorrection& correction,
                                         Timestamp received_at);

} // namespace tmcmt::pipeline
