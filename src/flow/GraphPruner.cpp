#include "trafficmcmt/flow/GraphPruner.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tmcmt::flow {
namespace {

class UnionFind {
public:
    explicit UnionFind(std::size_t size) : m_parent(size), m_rank(size, 0) {
        std::iota(m_parent.begin(), m_parent.end(), std::size_t{0});
    }

    std::size_t Find(std::size_t index) {
        std::size_t root = index;
        while (m_parent[root] != root) {
            root = m_parent[root];
        }
        while (m_parent[index] != index) {
            const std::size_t next = m_parent[index];
            m_parent[index] = root;
            index = next;
        }
        return root;
    }

    void Unite(std::size_t lhs, std::size_t rhs) {
        lhs = Find(lhs);
        rhs = Find(rhs);
        if (lhs == rhs) {
            return;
        }
        if (m_rank[lhs] < m_rank[rhs]) {
            std::swap(lhs, rhs);
        }
        m_parent[rhs] = lhs;
        if (m_rank[lhs] == m_rank[rhs]) {
            ++m_rank[lhs];
        }
    }

private:
    std::vector<std::size_t> m_parent;
    std::vector<std::size_t> m_rank;
};

bool EdgeLess(const TransitionEdge& lhs, const TransitionEdge& rhs) {
    if (lhs.from != rhs.from) {
        return lhs.from < rhs.from;
    }
    if (lhs.cost != rhs.cost) {
        return lhs.cost < rhs.cost;
    }
    return lhs.to < rhs.to;
}

} // namespace

GraphPruner::GraphPruner(GraphPruneOptions options) : m_options(options) {}

TrackletGraph GraphPruner::Prune(const TrackletGraph& graph, GraphPruneStats* stats) const {
    GraphPruneStats local;
    local.input_nodes = graph.NodeCount();
    local.input_edges = graph.EdgeCount();

    std::vector<TransitionEdge> edges;
    if (m_options.dedupe) {
        std::map<std::pair<TrackletId, TrackletId>, double> best;
        for (const TransitionEdge& edge : graph.Edges()) {
            const auto key = std::make_pair(edge.from, edge.to);
            const auto it = best.find(key);
            if (it == best.end()) {
                best.emplace(key, edge.cost);
            } else {
                ++local.duplicate_edges;
                if (edge.cost < it->second) {
                    it->second = edge.cost;
                }
            }
        }
        edges.reserve(best.size());
        for (const auto& [key, cost] : best) {
            edges.push_back(TransitionEdge{key.first, key.second, cost});
        }
    } else {
        edges = graph.Edges();
    }

    std::sort(edges.begin(), edges.end(), EdgeLess);

    std::vector<TransitionEdge> kept;
    kept.reserve(edges.size());
    std::size_t begin = 0;
    while (begin < edges.size()) {
        std::size_t end = begin;
        while (end < edges.size() && edges[end].from == edges[begin].from) {
            ++end;
        }
        const std::size_t count = end - begin;
        const std::size_t limit =
            m_options.max_candidates_per_node == 0
                ? count
                : std::min(count, m_options.max_candidates_per_node);
        for (std::size_t i = 0; i < limit; ++i) {
            kept.push_back(edges[begin + i]);
        }
        local.truncated_edges += count - limit;
        begin = end;
    }

    TrackletGraph pruned;
    pruned.SetLinkReward(graph.LinkReward());
    for (const TrackletNode& node : graph.Nodes()) {
        static_cast<void>(pruned.AddNode(node));
    }
    for (const TransitionEdge& edge : kept) {
        static_cast<void>(pruned.AddTransition(edge));
    }

    local.output_nodes = pruned.NodeCount();
    local.output_edges = pruned.EdgeCount();
    local.pruned_edges =
        local.input_edges >= local.output_edges ? local.input_edges - local.output_edges : 0;
    if (stats != nullptr) {
        *stats = local;
    }
    return pruned;
}

std::vector<TrackletGraph> GraphPruner::Split(const TrackletGraph& graph,
                                              GraphPruneStats* stats) const {
    GraphPruneStats local;
    const TrackletGraph pruned = Prune(graph, &local);

    const std::vector<TrackletNode>& nodes = pruned.Nodes();
    UnionFind unionFind(nodes.size());
    for (const TransitionEdge& edge : pruned.Edges()) {
        const auto from = pruned.IndexOf(edge.from);
        const auto to = pruned.IndexOf(edge.to);
        if (from && to) {
            unionFind.Unite(*from, *to);
        }
    }

    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        groups[unionFind.Find(index)].push_back(index);
    }

    std::vector<TrackletGraph> components;
    components.reserve(groups.size());
    for (const auto& [root, indices] : groups) {
        static_cast<void>(root);
        ++local.components;
        if (m_options.max_component_nodes != 0 &&
            indices.size() > m_options.max_component_nodes) {
            ++local.skipped_components;
            local.skipped_nodes += indices.size();
            continue;
        }

        TrackletGraph component;
        component.SetLinkReward(pruned.LinkReward());
        std::unordered_map<TrackletId, std::size_t> members;
        members.reserve(indices.size());
        for (const std::size_t index : indices) {
            static_cast<void>(component.AddNode(nodes[index]));
            members.emplace(nodes[index].id, index);
        }
        for (const TransitionEdge& edge : pruned.Edges()) {
            if (members.find(edge.from) != members.end() &&
                members.find(edge.to) != members.end()) {
                static_cast<void>(component.AddTransition(edge));
            }
        }
        components.push_back(std::move(component));
    }

    std::sort(components.begin(), components.end(),
              [](const TrackletGraph& lhs, const TrackletGraph& rhs) {
                  return lhs.Nodes().front().id < rhs.Nodes().front().id;
              });

    local.output_nodes = 0;
    local.output_edges = 0;
    for (const TrackletGraph& component : components) {
        local.output_nodes += component.NodeCount();
        local.output_edges += component.EdgeCount();
    }
    if (stats != nullptr) {
        *stats = local;
    }
    return components;
}

} // namespace tmcmt::flow
