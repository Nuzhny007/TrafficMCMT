#include "trafficmcmt/pipeline/ClockSyncEstimator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace tmcmt::pipeline {
namespace {

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    const double upper = values[mid];
    if (values.size() % 2 != 0) {
        return upper;
    }
    const double lower = *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lower + upper);
}

double Clamp(double value, double limit) {
    return std::clamp(value, -limit, limit);
}

} // namespace

Timestamp ClockCorrection::Apply(Timestamp edge) const noexcept {
    const double delta = static_cast<double>((edge - reference).count());
    const double shift = offset_ms + drift_ppm * 1e-6 * delta;
    return edge + Timestamp(static_cast<std::int64_t>(std::llround(shift)));
}

Timestamp ClockCorrection::Revert(Timestamp master) const noexcept {
    const double k = drift_ppm * 1e-6;
    const double denominator = 1.0 + k;
    if (std::abs(denominator) < 1e-12) {
        return master - Timestamp(static_cast<std::int64_t>(std::llround(offset_ms)));
    }
    const double referenceMs = static_cast<double>(reference.count());
    const double masterMs = static_cast<double>(master.count());
    const double edgeMs = (masterMs - offset_ms + k * referenceMs) / denominator;
    return Timestamp(static_cast<std::int64_t>(std::llround(edgeMs)));
}

ClockSyncEstimator::ClockSyncEstimator(ClockSyncOptions options) : m_options(std::move(options)) {
    if (m_options.window_samples == 0) {
        m_options.window_samples = 1;
    }
    if (m_options.min_samples == 0) {
        m_options.min_samples = 1;
    }
    if (m_options.min_samples > m_options.window_samples) {
        m_options.min_samples = m_options.window_samples;
    }
    if (m_options.max_offset_ms < 0) {
        m_options.max_offset_ms = 0;
    }
    if (m_options.max_drift_ppm < 0.0) {
        m_options.max_drift_ppm = 0.0;
    }
}

Status ClockSyncEstimator::AddSample(Timestamp edge_ms, Timestamp master_ms) {
    const double delta = static_cast<double>((master_ms - edge_ms).count()) -
                         static_cast<double>(m_options.assumed_transport_ms);
    m_samples.push_back(Sample{edge_ms, delta});
    while (m_samples.size() > m_options.window_samples) {
        m_samples.pop_front();
    }
    Recompute();
    return Status::Ok();
}

void ClockSyncEstimator::Recompute() {
    m_stats.samples = m_samples.size();
    if (m_samples.empty()) {
        m_correction = ClockCorrection{};
        m_stats = ClockSyncStats{};
        m_synced = false;
        return;
    }

    std::vector<double> deltas;
    deltas.reserve(m_samples.size());
    for (const Sample& sample : m_samples) {
        deltas.push_back(sample.delta_ms);
    }
    const double median = Median(deltas);

    std::vector<double> deviations;
    deviations.reserve(deltas.size());
    for (const double delta : deltas) {
        deviations.push_back(std::abs(delta - median));
    }
    const double mad = Median(deviations);
    const double sigma = 1.4826 * mad;

    std::vector<std::size_t> kept;
    kept.reserve(m_samples.size());
    for (std::size_t i = 0; i < m_samples.size(); ++i) {
        if (m_options.outlier_sigma <= 0.0 || sigma <= 0.0 ||
            std::abs(m_samples[i].delta_ms - median) <= m_options.outlier_sigma * sigma) {
            kept.push_back(i);
        }
    }
    if (kept.empty()) {
        kept.resize(m_samples.size());
        for (std::size_t i = 0; i < kept.size(); ++i) {
            kept[i] = i;
        }
    }
    m_stats.accepted = kept.size();

    // Theil-Sen slope: the median of all pairwise slopes.
    std::vector<double> slopes;
    slopes.reserve(kept.size() * (kept.size() - 1) / 2);
    for (std::size_t i = 0; i < kept.size(); ++i) {
        for (std::size_t j = i + 1; j < kept.size(); ++j) {
            const auto deltaEdge = (m_samples[kept[j]].edge - m_samples[kept[i]].edge).count();
            if (deltaEdge == 0) {
                continue;
            }
            const double deltaDelta = m_samples[kept[j]].delta_ms - m_samples[kept[i]].delta_ms;
            slopes.push_back(deltaDelta / static_cast<double>(deltaEdge));
        }
    }
    double slope = slopes.empty() ? 0.0 : Median(slopes);
    slope = Clamp(slope, m_options.max_drift_ppm * 1e-6);

    const Timestamp reference = m_samples[kept.back()].edge;
    std::vector<double> intercepts;
    intercepts.reserve(kept.size());
    for (const std::size_t index : kept) {
        const double deltaEdge = static_cast<double>((m_samples[index].edge - reference).count());
        intercepts.push_back(m_samples[index].delta_ms - slope * deltaEdge);
    }
    double intercept = Median(intercepts);
    intercept = Clamp(intercept, static_cast<double>(m_options.max_offset_ms));

    std::vector<double> residuals;
    residuals.reserve(kept.size());
    for (const std::size_t index : kept) {
        const double deltaEdge = static_cast<double>((m_samples[index].edge - reference).count());
        residuals.push_back(std::abs(m_samples[index].delta_ms - (intercept + slope * deltaEdge)));
    }

    m_correction.reference = reference;
    m_correction.offset_ms = intercept;
    m_correction.drift_ppm = slope * 1e6;

    m_stats.offset_ms = intercept;
    m_stats.drift_ppm = slope * 1e6;
    m_stats.residual_ms = Median(residuals);
    m_synced = m_samples.size() >= m_options.min_samples;
}

ClockCorrection ClockSyncEstimator::Correction(Timestamp edge_now) const noexcept {
    ClockCorrection correction;
    correction.reference = edge_now;
    correction.drift_ppm = m_correction.drift_ppm;
    correction.offset_ms =
        m_correction.offset_ms +
        m_correction.drift_ppm * 1e-6 * static_cast<double>((edge_now - m_correction.reference).count());
    return correction;
}

ClockSyncStats ClockSyncEstimator::Stats() const noexcept {
    ClockSyncStats stats = m_stats;
    stats.synced = m_synced;
    return stats;
}

void ClockSyncEstimator::Reset() {
    m_samples.clear();
    m_correction = ClockCorrection{};
    m_stats = ClockSyncStats{};
    m_synced = false;
}

SyncedTracklet Synchronize(Tracklet tracklet, const ClockCorrection& correction, Timestamp received_at) {
    SyncedTracklet synced;
    synced.raw_begin = tracklet.begin;
    synced.raw_end = tracklet.end;
    synced.correction = correction;
    synced.received_at = received_at;

    synced.tracklet = std::move(tracklet);
    synced.tracklet.begin = correction.Apply(synced.raw_begin);
    synced.tracklet.end = correction.Apply(synced.raw_end);
    for (Observation& observation : synced.tracklet.observations) {
        observation.ts = correction.Apply(observation.ts);
    }
    return synced;
}

} // namespace tmcmt::pipeline
