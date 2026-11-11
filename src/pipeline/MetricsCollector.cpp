#include "trafficmcmt/pipeline/MetricsCollector.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace tmcmt::pipeline {
namespace {

double Percentile(const std::vector<std::int64_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto index = static_cast<std::size_t>(position);
    const std::size_t next = std::min(index + 1, sorted.size() - 1);
    const double weight = position - static_cast<double>(index);
    return static_cast<double>(sorted[index]) * (1.0 - weight) + static_cast<double>(sorted[next]) * weight;
}

} // namespace

MetricsCollector::Ring::Ring(std::size_t capacity)
    : m_values(capacity == 0 ? 1 : capacity), m_capacity(capacity == 0 ? 1 : capacity) {}

void MetricsCollector::Ring::Add(std::int64_t value) {
    m_values[m_pos] = value;
    m_pos = (m_pos + 1) % m_capacity;
    if (m_count < m_capacity) {
        ++m_count;
    }
}

LatencyStats MetricsCollector::Ring::Compute() const {
    LatencyStats stats;
    if (m_count == 0) {
        return stats;
    }

    std::vector<std::int64_t> sorted;
    sorted.reserve(m_count);
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < m_count; ++i) {
        sorted.push_back(m_values[i]);
        sum += m_values[i];
    }
    std::sort(sorted.begin(), sorted.end());

    stats.count = m_count;
    stats.mean_ms = static_cast<double>(sum) / static_cast<double>(m_count);
    stats.p50_ms = Percentile(sorted, 0.50);
    stats.p95_ms = Percentile(sorted, 0.95);
    stats.p99_ms = Percentile(sorted, 0.99);
    stats.max_ms = static_cast<double>(sorted.back());
    return stats;
}

void MetricsCollector::Ring::Clear() {
    m_pos = 0;
    m_count = 0;
}

MetricsCollector::MetricsCollector(std::size_t capacity)
    : m_ingest(capacity), m_lateness(capacity), m_solve(capacity) {}

void MetricsCollector::RecordIngestLatency(Timestamp latency) {
    m_ingest.Add(latency.count());
}

void MetricsCollector::RecordLateness(Timestamp lateness) {
    m_lateness.Add(lateness.count());
}

void MetricsCollector::RecordSolveTime(Timestamp duration) {
    m_solve.Add(duration.count());
}

PipelineStats MetricsCollector::Snapshot() const {
    PipelineStats stats;
    stats.counters = m_counters;
    stats.ingest_latency_ms = m_ingest.Compute();
    stats.lateness_ms = m_lateness.Compute();
    stats.solve_ms = m_solve.Compute();
    stats.cameras = m_cameras;
    stats.buffered = m_buffered;
    stats.tracklets = m_tracklets;
    stats.gallery = m_gallery;
    return stats;
}

void MetricsCollector::Reset() {
    m_ingest.Clear();
    m_lateness.Clear();
    m_solve.Clear();
    m_counters = PipelineCounters{};
    m_cameras.clear();
    m_buffered = 0;
    m_tracklets = 0;
    m_gallery = 0;
}

} // namespace tmcmt::pipeline
