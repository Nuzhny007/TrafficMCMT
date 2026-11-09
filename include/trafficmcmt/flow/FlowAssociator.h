#pragma once

#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/flow/GlobalIdManager.h"
#include "trafficmcmt/flow/McfSolver.h"
#include "trafficmcmt/flow/TrackletGraph.h"

namespace tmcmt::flow {

struct FlowAssociationOptions {
    SolverKind solver = SolverKind::Ssp;
    /// Transition cost is `c_total - link_reward`; a transition is selected
    /// only when this value is negative.
    double link_reward = 1.0;
    std::size_t max_candidates_per_node = 15;
    std::size_t max_component_nodes = 20000;
    bool split_components = true;
    /// Passed to `GlobalIdManager` for online commit semantics. Offline runs
    /// may leave it at the default; the assignment is unaffected.
    Timestamp commit_lag{std::chrono::milliseconds(15000)};
};

struct FlowAssociationStats {
    std::size_t tracklets = 0;
    std::size_t transitions = 0;
    std::size_t rejected_transitions = 0;
    std::size_t duplicate_edges = 0;
    std::size_t pruned_edges = 0;
    std::size_t components = 0;
    std::size_t skipped_components = 0;
    std::size_t augmentations = 0;
    std::size_t paths = 0;
};

struct FlowAssociationResult {
    std::vector<FlowPath> paths;
    std::unordered_map<TrackletId, GlobalId> assignments;
    std::vector<GlobalIdMergeEvent> merge_events;
    double total_cost = 0.0;
    FlowAssociationStats stats;
    bool optimal = false;
};

/// End-to-end offline association: build graph, prune, split into components,
/// solve each component and turn paths into global ids.
class FlowAssociator {
public:
    explicit FlowAssociator(FlowAssociationOptions options = {});

    [[nodiscard]] Result<FlowAssociationResult> Associate(
        std::span<const Tracklet> tracklets,
        std::span<const TransitionEdge> transitions) const;

    [[nodiscard]] Result<FlowAssociationResult> Associate(const TrackletGraph& graph) const;

    [[nodiscard]] const FlowAssociationOptions& Options() const noexcept {
        return m_options;
    }

private:
    FlowAssociationOptions m_options;
};

} // namespace tmcmt::flow
