#include "trafficmcmt/flow/TrackletGraph.h"

#include <cmath>
#include <utility>

namespace tmcmt::flow {
namespace {

Status Invalid(std::string message) {
    return Status::Error(ErrorCode::InvalidArgument, std::move(message));
}

} // namespace

Status TrackletGraph::AddNode(const Tracklet& tracklet) {
    TrackletNode node;
    node.id = tracklet.id;
    node.camera = tracklet.camera;
    node.begin = tracklet.begin;
    node.end = tracklet.end;
    node.type = tracklet.type;
    node.quality = tracklet.quality;
    return AddNode(node);
}

Status TrackletGraph::AddNode(const TrackletNode& node) {
    if (node.id == 0) {
        return Invalid("tracklet id must be non-zero");
    }
    if (node.end < node.begin) {
        return Invalid("tracklet end precedes begin");
    }
    if (m_index.find(node.id) != m_index.end()) {
        return Invalid("duplicate tracklet id " + std::to_string(node.id));
    }
    m_index.emplace(node.id, m_nodes.size());
    m_nodes.push_back(node);
    return Status::Ok();
}

Status TrackletGraph::AddTransition(const TransitionEdge& edge) {
    return AddTransition(edge.from, edge.to, edge.cost);
}

Status TrackletGraph::AddTransition(TrackletId from, TrackletId to, double cost) {
    if (from == 0 || to == 0) {
        return Invalid("transition endpoints must be non-zero");
    }
    if (from == to) {
        return Invalid("self transition is not allowed");
    }
    if (!std::isfinite(cost)) {
        return Invalid("transition cost must be finite");
    }

    const auto fromIt = m_index.find(from);
    const auto toIt = m_index.find(to);
    if (fromIt == m_index.end()) {
        return Status::Error(ErrorCode::NotFound,
                             "unknown source tracklet " + std::to_string(from));
    }
    if (toIt == m_index.end()) {
        return Status::Error(ErrorCode::NotFound,
                             "unknown target tracklet " + std::to_string(to));
    }

    const TrackletNode& source = m_nodes[fromIt->second];
    const TrackletNode& target = m_nodes[toIt->second];
    if (source.end > target.begin) {
        return Invalid("transition " + std::to_string(from) + " -> " + std::to_string(to) +
                       " is not forward in time");
    }
    if (source.end == target.begin && source.id >= target.id) {
        return Invalid("transition " + std::to_string(from) + " -> " + std::to_string(to) +
                       " would create a cycle");
    }

    m_edges.push_back(TransitionEdge{from, to, cost});
    return Status::Ok();
}

const TrackletNode* TrackletGraph::FindNode(TrackletId id) const noexcept {
    const auto it = m_index.find(id);
    if (it == m_index.end()) {
        return nullptr;
    }
    return &m_nodes[it->second];
}

std::optional<std::size_t> TrackletGraph::IndexOf(TrackletId id) const noexcept {
    const auto it = m_index.find(id);
    if (it == m_index.end()) {
        return std::nullopt;
    }
    return it->second;
}

void TrackletGraph::Clear() noexcept {
    m_nodes.clear();
    m_edges.clear();
    m_index.clear();
    m_linkReward = 1.0;
}

} // namespace tmcmt::flow
