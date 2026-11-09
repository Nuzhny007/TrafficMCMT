#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::flow {

/// Immutable metadata of one tracklet inside the association graph.
struct TrackletNode {
    TrackletId id = 0;
    CameraId camera;
    Timestamp begin{};
    Timestamp end{};
    ObjectType type = ObjectType::Unknown;
    float quality = 1.f;
};

/// Directed transition `out(from) -> in(to)`.
///
/// `cost` is the fused st-ReID cost `c_total` (in [0, 2]). The MCF solvers
/// subtract `TrackletGraph::LinkReward()` when they build the network so that
/// a likely transition becomes a negative-cost edge.
struct TransitionEdge {
    TrackletId from = 0;
    TrackletId to = 0;
    double cost = 0.0;
};

/// Tracklet association graph without capacities or solver-specific layout.
///
/// The class validates identifiers and enforces the time ordering that makes
/// the graph acyclic: a transition may only go from a tracklet that ends no
/// later than the next one begins. Equal timestamps are broken by tracklet id
/// so that a pair of zero-length tracklets cannot produce a 2-cycle.
class TrackletGraph {
public:
    TrackletGraph() = default;

    [[nodiscard]] Status AddNode(const Tracklet& tracklet);
    [[nodiscard]] Status AddNode(const TrackletNode& node);

    [[nodiscard]] Status AddTransition(const TransitionEdge& edge);
    [[nodiscard]] Status AddTransition(TrackletId from, TrackletId to, double cost);

    [[nodiscard]] const std::vector<TrackletNode>& Nodes() const noexcept {
        return m_nodes;
    }
    [[nodiscard]] const std::vector<TransitionEdge>& Edges() const noexcept {
        return m_edges;
    }

    [[nodiscard]] const TrackletNode* FindNode(TrackletId id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> IndexOf(TrackletId id) const noexcept;

    [[nodiscard]] std::size_t NodeCount() const noexcept {
        return m_nodes.size();
    }
    [[nodiscard]] std::size_t EdgeCount() const noexcept {
        return m_edges.size();
    }
    [[nodiscard]] bool Empty() const noexcept {
        return m_nodes.empty();
    }

    /// Reward subtracted from every transition cost by the MCF solvers.
    void SetLinkReward(double value) noexcept {
        m_linkReward = value;
    }
    [[nodiscard]] double LinkReward() const noexcept {
        return m_linkReward;
    }

    void Clear() noexcept;

private:
    std::vector<TrackletNode> m_nodes;
    std::vector<TransitionEdge> m_edges;
    std::unordered_map<TrackletId, std::size_t> m_index;
    double m_linkReward = 1.0;
};

} // namespace tmcmt::flow
