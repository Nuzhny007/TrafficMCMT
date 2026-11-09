#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::flow {

struct GlobalIdManagerOptions {
    /// An identity is frozen once all its tracklets ended more than this lag
    /// before the commit timestamp.
    Timestamp commit_lag{std::chrono::milliseconds(15000)};
    /// First global id handed out. Must be non-zero; 0 selects 1.
    GlobalId first_id = 1;
};

/// Emitted whenever two already assigned identities are merged.
struct GlobalIdMergeEvent {
    GlobalId old_id = kInvalidGlobalId;
    GlobalId new_id = kInvalidGlobalId;
    std::string reason;
    Timestamp ts{};
};

/// Stable, monotone global identity registry backed by a union-find.
///
/// * ids start at `first_id` and are never reused;
/// * merging two groups keeps the smaller id (or the committed one when only
///   one side is frozen) and records a `GlobalIdMergeEvent`;
/// * `Commit(now)` freezes groups whose last tracklet is older than
///   `now - commit_lag`; frozen ids are never changed by later links.
class GlobalIdManager {
public:
    explicit GlobalIdManager(GlobalIdManagerOptions options = {});

    /// Registers a tracklet and returns its current global id. Re-adding the
    /// same tracklet only extends its end timestamp.
    [[nodiscard]] Result<GlobalId> Add(TrackletId id, Timestamp end);

    /// Links two tracklets into one identity. Unknown ids are an error; a
    /// repeated link is a no-op.
    [[nodiscard]] Status Link(TrackletId a, TrackletId b, Timestamp ts,
                              std::string reason = {});

    /// Freezes identities that have not been active since `now - commit_lag`.
    [[nodiscard]] Status Commit(Timestamp now);

    [[nodiscard]] GlobalId IdFor(TrackletId id) const noexcept;
    [[nodiscard]] bool Contains(TrackletId id) const noexcept;
    [[nodiscard]] bool IsCommitted(GlobalId id) const noexcept;

    [[nodiscard]] std::size_t Size() const noexcept {
        return m_elements.size();
    }
    [[nodiscard]] std::size_t GroupCount() const;

    [[nodiscard]] const std::vector<GlobalIdMergeEvent>& MergeEvents() const noexcept {
        return m_events;
    }

    /// `(tracklet id, global id)` pairs sorted by tracklet id.
    [[nodiscard]] std::vector<std::pair<TrackletId, GlobalId>> Assignments() const;

    void Clear();

private:
    struct Element {
        std::size_t parent = 0;
        std::size_t rank = 0;
        GlobalId global_id = kInvalidGlobalId;
        Timestamp max_end{};
        bool committed = false;
    };

    [[nodiscard]] std::size_t Find(std::size_t index);
    [[nodiscard]] std::size_t Find(std::size_t index) const;

    GlobalIdManagerOptions m_options;
    GlobalId m_next_id = 1;
    std::vector<Element> m_elements;
    std::unordered_map<TrackletId, std::size_t> m_index;
    std::unordered_set<GlobalId> m_committed_ids;
    std::vector<GlobalIdMergeEvent> m_events;
};

} // namespace tmcmt::flow
