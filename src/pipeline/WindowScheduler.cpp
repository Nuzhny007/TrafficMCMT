#include "trafficmcmt/pipeline/WindowScheduler.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace tmcmt::pipeline {
namespace {

std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) {
    std::int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

} // namespace

WindowScheduler::WindowScheduler(WindowOptions options) : m_options(options) {
    if (m_options.window_ms <= 0) {
        m_options.window_ms = 30'000;
    }
    if (m_options.overlap_ms < 0 || m_options.overlap_ms >= m_options.window_ms) {
        m_options.overlap_ms = 0;
    }
    if (m_options.warmup_ms < 0) {
        m_options.warmup_ms = 0;
    }
    if (m_options.commit_lag_ms < 0) {
        m_options.commit_lag_ms = 0;
    }
}

void WindowScheduler::Start(Timestamp first_event) {
    if (m_started) {
        return;
    }
    const std::int64_t window = m_options.window_ms;
    const std::int64_t aligned = FloorDiv(first_event.count(), window) * window;
    m_grid_start = Timestamp(aligned);
    m_next_begin = m_grid_start;
    m_first_event = first_event;
    m_next_index = 0;
    m_started = true;
}

std::vector<Window> WindowScheduler::Advance(Timestamp watermark) {
    std::vector<Window> out;
    if (!m_started) {
        return out;
    }

    const Timestamp warmup_end = m_first_event + Timestamp(m_options.warmup_ms);
    for (;;) {
        const Timestamp end = m_next_begin + Timestamp(m_options.window_ms);
        if (watermark < end) {
            break;
        }
        if (m_next_index == 0 && watermark < warmup_end) {
            break;
        }
        out.push_back(Window{m_next_begin, end, m_next_index});
        ++m_stats.windows_emitted;
        m_next_begin = m_next_begin + Timestamp(Step());
        ++m_next_index;
    }
    return out;
}

std::vector<Window> WindowScheduler::AdvanceCommit(Timestamp watermark) {
    std::vector<Window> out;
    if (!m_started) {
        return out;
    }

    while (m_commit_cursor < m_next_index && m_committed.count(m_commit_cursor) != 0) {
        ++m_commit_cursor;
    }

    const Timestamp lag(m_options.commit_lag_ms);
    for (std::uint64_t index = m_commit_cursor; index < m_next_index; ++index) {
        const Window window = WindowFor(index);
        if (watermark < window.end + lag) {
            break;
        }
        out.push_back(window);
    }
    return out;
}

std::uint64_t WindowScheduler::IndexFor(Timestamp event_time) const noexcept {
    if (event_time < m_grid_start) {
        return 0;
    }
    const std::int64_t step = Step();
    if (step <= 0) {
        return 0;
    }
    const std::int64_t delta = (event_time - m_grid_start).count();
    return static_cast<std::uint64_t>(delta / step);
}

Window WindowScheduler::WindowFor(std::uint64_t index) const noexcept {
    Window window;
    window.index = index;
    window.begin = m_grid_start + Timestamp(static_cast<std::int64_t>(index) * Step());
    window.end = window.begin + Timestamp(m_options.window_ms);
    return window;
}

WindowState WindowScheduler::StateFor(Timestamp event_time) const {
    if (!m_started || event_time < m_grid_start) {
        return WindowState::Pending;
    }
    const std::uint64_t index = IndexFor(event_time);
    if (m_committed.count(index) != 0) {
        return WindowState::Committed;
    }
    if (index >= m_next_index) {
        return WindowState::Pending;
    }
    return WindowState::Emitted;
}

std::optional<Window> WindowScheduler::ReopenFor(Timestamp event_time) {
    if (!m_started || event_time < m_grid_start) {
        return std::nullopt;
    }
    const std::uint64_t index = IndexFor(event_time);
    if (index >= m_next_index || m_committed.count(index) != 0) {
        return std::nullopt;
    }
    ++m_stats.windows_reopened;
    return WindowFor(index);
}

void WindowScheduler::MarkCommitted(std::uint64_t index) {
    if (m_committed.insert(index).second) {
        ++m_stats.windows_committed;
    }
    // Windows are 30 s by default; 100k entries cover more than a month of
    // continuous operation. Prune the oldest half when the bound is reached.
    if (m_committed.size() > 100'000) {
        auto it = m_committed.begin();
        for (std::size_t i = 0; i < 50'000 && it != m_committed.end(); ++i) {
            it = m_committed.erase(it);
        }
    }
}

bool WindowScheduler::IsCommitted(std::uint64_t index) const noexcept {
    return m_committed.count(index) != 0;
}

void WindowScheduler::Reset() {
    m_grid_start = Timestamp{};
    m_next_begin = Timestamp{};
    m_first_event = Timestamp{};
    m_next_index = 0;
    m_commit_cursor = 0;
    m_started = false;
    m_committed.clear();
    m_stats = WindowSchedulerStats{};
}

} // namespace tmcmt::pipeline
