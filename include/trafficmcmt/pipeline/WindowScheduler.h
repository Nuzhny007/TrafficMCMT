#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "trafficmcmt/core/Types.h"

namespace tmcmt::pipeline {

struct WindowOptions {
    std::int64_t window_ms = 30'000;
    std::int64_t overlap_ms = 10'000;
    /// Minimum event time that must pass before the first window may close.
    /// Buffers the beginning of the stream so the gallery and the clock
    /// estimator can warm up.
    std::int64_t warmup_ms = 10'000;
    /// A solved window stays reopenable for this long.
    std::int64_t commit_lag_ms = 15'000;
};

struct Window {
    Timestamp begin{};
    Timestamp end{};
    std::uint64_t index = 0;

    [[nodiscard]] bool operator==(const Window& other) const noexcept {
        return begin == other.begin && end == other.end && index == other.index;
    }
};

/// Position of an event relative to the emission cursor.
enum class WindowState : std::uint8_t {
    /// The window has not been solved yet; a late event is still included when
    /// the window closes.
    Pending = 0,
    /// The window was solved but can still be reopened.
    Emitted = 1,
    /// The window is frozen; late data is dropped.
    Committed = 2,
};

struct WindowSchedulerStats {
    std::uint64_t windows_emitted = 0;
    std::uint64_t windows_reopened = 0;
    std::uint64_t windows_committed = 0;
};

/// Turns a monotone watermark into a deterministic sequence of overlapping
/// windows.
///
/// Window `k` covers `[begin_k, begin_k + window_ms)`, with
/// `begin_{k+1} = begin_k + window_ms - overlap_ms`. A window is *emitted* when
/// the watermark reaches its end; it becomes *committed* `commit_lag` later.
/// Late data may reopen an emitted-but-not-committed window, but never a
/// committed one.
class WindowScheduler {
public:
    explicit WindowScheduler(WindowOptions options = {});

    /// Aligns the grid to `first_event`. Idempotent until `Reset()`.
    void Start(Timestamp first_event);

    [[nodiscard]] bool started() const noexcept {
        return m_started;
    }

    /// Returns every window that the watermark has closed since the last call.
    [[nodiscard]] std::vector<Window> Advance(Timestamp watermark);

    /// Returns solved windows whose `end + commit_lag` is now behind the
    /// watermark. They must be marked with `MarkCommitted` by the caller.
    [[nodiscard]] std::vector<Window> AdvanceCommit(Timestamp watermark);

    [[nodiscard]] WindowState StateFor(Timestamp event_time) const;

    /// Window containing `event_time` when it was already emitted and is not
    /// committed yet. Returns `nullopt` for pending or committed windows.
    [[nodiscard]] std::optional<Window> ReopenFor(Timestamp event_time);

    /// Marks a window as frozen. Later `ReopenFor` calls will reject it.
    void MarkCommitted(std::uint64_t index);

    [[nodiscard]] bool IsCommitted(std::uint64_t index) const noexcept;

    [[nodiscard]] Window WindowFor(std::uint64_t index) const noexcept;

    [[nodiscard]] Timestamp next_begin() const noexcept {
        return m_next_begin;
    }

    [[nodiscard]] std::uint64_t next_index() const noexcept {
        return m_next_index;
    }

    [[nodiscard]] const WindowSchedulerStats& stats() const noexcept {
        return m_stats;
    }

    void Reset();

private:
    [[nodiscard]] std::int64_t Step() const noexcept {
        return m_options.window_ms - m_options.overlap_ms;
    }
    [[nodiscard]] std::uint64_t IndexFor(Timestamp event_time) const noexcept;

    WindowOptions m_options;
    Timestamp m_grid_start{};
    Timestamp m_next_begin{};
    Timestamp m_first_event{};
    std::uint64_t m_next_index = 0;
    std::uint64_t m_commit_cursor = 0;
    bool m_started = false;
    std::set<std::uint64_t> m_committed;
    WindowSchedulerStats m_stats;
};

} // namespace tmcmt::pipeline
