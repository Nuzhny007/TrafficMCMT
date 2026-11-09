#include "trafficmcmt/flow/FlowAssociator.h"

#include <algorithm>
#include <set>
#include <utility>

#include "trafficmcmt/flow/GraphPruner.h"

namespace tmcmt::flow {

FlowAssociator::FlowAssociator(FlowAssociationOptions options) : m_options(options) {}

Result<FlowAssociationResult> FlowAssociator::Associate(
    std::span<const Tracklet> tracklets, std::span<const TransitionEdge> transitions) const {
    TrackletGraph graph;
    graph.SetLinkReward(m_options.link_reward);
    for (const Tracklet& tracklet : tracklets) {
        Status status = graph.AddNode(tracklet);
        if (!status.ok()) {
            return status;
        }
    }

    FlowAssociationStats inputStats;
    inputStats.tracklets = tracklets.size();
    inputStats.transitions = transitions.size();
    for (const TransitionEdge& edge : transitions) {
        Status status = graph.AddTransition(edge);
        if (!status.ok()) {
            ++inputStats.rejected_transitions;
        }
    }

    auto result = Associate(graph);
    if (!result) {
        return result.status();
    }
    result.value().stats.tracklets = inputStats.tracklets;
    result.value().stats.transitions = inputStats.transitions;
    result.value().stats.rejected_transitions = inputStats.rejected_transitions;
    return result;
}

Result<FlowAssociationResult> FlowAssociator::Associate(const TrackletGraph& graph) const {
    FlowAssociationResult result;
    result.stats.tracklets = graph.NodeCount();
    result.stats.transitions = graph.EdgeCount();
    if (graph.Empty()) {
        result.optimal = true;
        return result;
    }

    GraphPruneOptions pruneOptions;
    pruneOptions.max_candidates_per_node = m_options.max_candidates_per_node;
    pruneOptions.max_component_nodes = m_options.max_component_nodes;
    pruneOptions.dedupe = true;
    const GraphPruner pruner(pruneOptions);

    GraphPruneStats pruneStats;
    std::vector<TrackletGraph> components;
    if (m_options.split_components) {
        components = pruner.Split(graph, &pruneStats);
    } else {
        components.push_back(pruner.Prune(graph, &pruneStats));
    }

    result.stats.components = pruneStats.components;
    result.stats.skipped_components = pruneStats.skipped_components;
    result.stats.pruned_edges = pruneStats.pruned_edges;
    result.stats.duplicate_edges = pruneStats.duplicate_edges;

    std::vector<FlowPath> paths;
    for (const TrackletGraph& component : components) {
        if (component.Empty()) {
            continue;
        }
        auto solver = CreateMcfSolver(m_options.solver);
        if (!solver) {
            return solver.status();
        }
        auto solved = solver.value()->Solve(component);
        if (!solved) {
            return solved.status();
        }
        result.stats.augmentations += solved.value().augmentations;
        result.total_cost += solved.value().total_cost;
        for (FlowPath& path : solved.value().paths) {
            paths.push_back(std::move(path));
        }
    }

    std::set<TrackletId> covered;
    for (const FlowPath& path : paths) {
        covered.insert(path.tracklets.begin(), path.tracklets.end());
    }
    for (const TrackletNode& node : graph.Nodes()) {
        if (covered.find(node.id) == covered.end()) {
            FlowPath path;
            path.tracklets.push_back(node.id);
            paths.push_back(std::move(path));
        }
    }

    std::sort(paths.begin(), paths.end(), [](const FlowPath& lhs, const FlowPath& rhs) {
        return lhs.tracklets.front() < rhs.tracklets.front();
    });
    result.stats.paths = paths.size();
    result.paths = std::move(paths);
    result.optimal =
        m_options.solver != SolverKind::Greedy && result.stats.skipped_components == 0;

    std::vector<const TrackletNode*> ordered;
    ordered.reserve(graph.NodeCount());
    for (const TrackletNode& node : graph.Nodes()) {
        ordered.push_back(&node);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const TrackletNode* lhs, const TrackletNode* rhs) {
                  if (lhs->begin != rhs->begin) {
                      return lhs->begin < rhs->begin;
                  }
                  return lhs->id < rhs->id;
              });

    GlobalIdManagerOptions managerOptions;
    managerOptions.commit_lag = m_options.commit_lag;
    managerOptions.first_id = 1;
    GlobalIdManager manager(managerOptions);
    for (const TrackletNode* node : ordered) {
        auto added = manager.Add(node->id, node->end);
        if (!added) {
            return added.status();
        }
    }
    for (const FlowPath& path : result.paths) {
        for (std::size_t index = 1; index < path.tracklets.size(); ++index) {
            const TrackletNode* target = graph.FindNode(path.tracklets[index]);
            const Timestamp timestamp = target == nullptr ? Timestamp{} : target->begin;
            Status status =
                manager.Link(path.tracklets[index - 1], path.tracklets[index], timestamp, "flow");
            if (!status.ok()) {
                return status;
            }
        }
    }

    for (const auto& [tracklet, globalId] : manager.Assignments()) {
        result.assignments.emplace(tracklet, globalId);
    }
    result.merge_events = manager.MergeEvents();
    return result;
}

} // namespace tmcmt::flow
