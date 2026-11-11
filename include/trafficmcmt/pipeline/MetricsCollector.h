#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "trafficmcmt/core/Types.h"

namespace tmcmt::pipeline {

struct LatencyStats {
    std::uint64_t count = 0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

struct CameraClockStats {
    CameraId camera;
    double offset_ms = 0.0;
    double drift_ppm = 0.0;
    double residual_ms = 0.0;
    std::size_t samples = 0;
    bool synced = false;
};

struct PipelineCounters {
    std::uint64_t ingested = 0;
    std::uint64_t accepted = 0;
    std::uint64_t late = 0;
    std::uint64_t too_late = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t dropped = 0;

    std::uint64_t windows_solved = 0;
    std::uint64_t windows_reopened = 0;
    std::uint64_t windows_committed = 0;

    std::uint64_t results_published = 0;
    std::uint64_t merges_published = 0;
    std::uint64_t sink_failures = 0;

    std::int64_t watermark_ms = 0;
};

struct PipelineStats {
    PipelineCounters counters;
    LatencyStats ingest_latency_ms;
    LatencyStats lateness_ms;
    LatencyStats solve_ms;
    std::vector<CameraClockStats> cameras;
    std::size_t buffered = 0;
    std::size_t tracklets = 0;
    std::size_t gallery = 0;
};

/// Fixed-capacity latency recorder plus pipeline counters.
///
/// Not thread-safe: the online loop is single-threaded and `Stats()` is called
/// from the same thread (or after `Stop()`).
class MetricsCollector {
public:
    explicit MetricsCollector(std::size_t capacity = 4'096);

    void RecordIngestLatency(Timestamp latency);
    void RecordLateness(Timestamp lateness);
    void RecordSolveTime(Timestamp duration);

    [[nodiscard]] PipelineCounters& counters() noexcept {
        return m_counters;
    }
    [[nodiscard]] const PipelineCounters& counters() const noexcept {
        return m_counters;
    }

    void SetWatermark(Timestamp watermark) noexcept {
        m_counters.watermark_ms = watermark.count();
    }

    void SetCameras(std::vector<CameraClockStats> cameras) {
        m_cameras = std::move(cameras);
    }

    void SetQueue(std::size_t buffered, std::size_t tracklets, std::size_t gallery) noexcept {
        m_buffered = buffered;
        m_tracklets = tracklets;
        m_gallery = gallery;
    }

    [[nodiscard]] PipelineStats Snapshot() const;

    void Reset();

private:
    class Ring {
    public:
        explicit Ring(std::size_t capacity);
        void Add(std::int64_t value);
        [[nodiscard]] LatencyStats Compute() const;
        void Clear();
        [[nodiscard]] bool empty() const noexcept {
            return m_count == 0;
        }

    private:
        std::vector<std::int64_t> m_values;
        std::size_t m_capacity = 0;
        std::size_t m_pos = 0;
        std::size_t m_count = 0;
    };

    Ring m_ingest;
    Ring m_lateness;
    Ring m_solve;
    PipelineCounters m_counters;
    std::vector<CameraClockStats> m_cameras;
    std::size_t m_buffered = 0;
    std::size_t m_tracklets = 0;
    std::size_t m_gallery = 0;
};

} // namespace tmcmt::pipeline
