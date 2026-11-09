#pragma once

#include <cstddef>
#include <vector>

#include "trafficmcmt/flow/TrackletGraph.h"

namespace tmcmt::flow {

struct GraphPruneOptions {
    /// Keep at most this many outgoing transitions per node. 0 disables the
    /// limit (useful for small graphs and tests).
    std::size_t max_candidates_per_node = 15;
    /// Components larger than this are dropped instead of being solved.
    /// 0 disables the limit.
    std::size_t max_component_nodes = 20000;
    /// Collapse duplicate `(from, to)` transitions keeping the cheapest one.
    bool dedupe = true;
};

struct GraphPruneStats {
    std::size_t input_nodes = 0;
    std::size_t input_edges = 0;
    std::size_t duplicate_edges = 0;
    std::size_t truncated_edges = 0;
    std::size_t pruned_edges = 0;
    std::size_t components = 0;
    std::size_t skipped_components = 0;
    std::size_t skipped_nodes = 0;
    std::size_t output_nodes = 0;
    std::size_t output_edges = 0;
};

/// Reduces the raw association graph before it reaches an MCF solver.
///
/// `Prune` removes duplicate edges and keeps the top-K outgoing transitions of
/// every node. `Split` additionally groups the pruned graph into weakly
/// connected components and drops the components that exceed
/// `max_component_nodes`.
class GraphPruner {
public:
    explicit GraphPruner(GraphPruneOptions options = {});

    [[nodiscard]] TrackletGraph Prune(const TrackletGraph& graph,
                                      GraphPruneStats* stats = nullptr) const;

    [[nodiscard]] std::vector<TrackletGraph> Split(const TrackletGraph& graph,
                                                   GraphPruneStats* stats = nullptr) const;

    [[nodiscard]] const GraphPruneOptions& Options() const noexcept {
        return m_options;
    }

private:
    GraphPruneOptions m_options;
};

} // namespace tmcmt::flow
