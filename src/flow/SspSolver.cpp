#include "trafficmcmt/flow/McfSolver.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tmcmt::flow {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

struct ResidualEdge {
    int to = 0;
    int reverse = 0;
    int capacity = 0;
    double cost = 0.0;
};

struct OriginalEdge {
    int from = 0;
    int to = 0;
    int adjacency_index = 0;
    int tracklet_from = -1;
    int tracklet_to = -1;
    bool transition = false;
    double cost = 0.0;
};

int AddEdge(std::vector<std::vector<ResidualEdge>>& adjacency, int from, int to, int capacity,
            double cost) {
    const int index = static_cast<int>(adjacency[static_cast<std::size_t>(from)].size());
    const int reverseIndex = static_cast<int>(adjacency[static_cast<std::size_t>(to)].size());
    adjacency[static_cast<std::size_t>(from)].push_back(
        ResidualEdge{to, reverseIndex, capacity, cost});
    adjacency[static_cast<std::size_t>(to)].push_back(
        ResidualEdge{from, index, 0, -cost});
    return index;
}

std::vector<FlowPath> BuildPaths(const TrackletGraph& graph,
                                 const std::vector<std::vector<ResidualEdge>>& adjacency,
                                 const std::vector<OriginalEdge>& originals,
                                 std::size_t nodeCount) {
    const std::vector<TrackletNode>& nodes = graph.Nodes();
    std::vector<int> nextTracklet(nodeCount, -1);
    std::vector<int> previousTracklet(nodeCount, -1);
    std::vector<double> nextCost(nodeCount, 0.0);

    for (const OriginalEdge& edge : originals) {
        if (!edge.transition || edge.tracklet_from < 0 || edge.tracklet_to < 0) {
            continue;
        }
        const ResidualEdge& residual =
            adjacency[static_cast<std::size_t>(edge.from)]
                     [static_cast<std::size_t>(edge.adjacency_index)];
        if (residual.capacity != 0) {
            continue;
        }
        const auto from = static_cast<std::size_t>(edge.tracklet_from);
        const auto to = static_cast<std::size_t>(edge.tracklet_to);
        nextTracklet[from] = edge.tracklet_to;
        nextCost[from] = edge.cost;
        previousTracklet[to] = edge.tracklet_from;
    }

    // Start only at nodes without a selected incoming transition. Node indices
    // follow the insertion order and are not necessarily topological, so a
    // path must be reconstructed from the selected transitions, not from
    // `source -> in` alone.
    std::vector<FlowPath> paths;
    std::vector<char> covered(nodeCount, 0);
    for (std::size_t index = 0; index < nodeCount; ++index) {
        if (previousTracklet[index] >= 0) {
            continue;
        }
        FlowPath path;
        std::size_t current = index;
        double cost = 0.0;
        while (current < nodeCount && covered[current] == 0) {
            covered[current] = 1;
            path.tracklets.push_back(nodes[current].id);
            const int next = nextTracklet[current];
            if (next < 0) {
                break;
            }
            cost += nextCost[current];
            current = static_cast<std::size_t>(next);
        }
        path.cost = cost;
        paths.push_back(std::move(path));
    }

    for (std::size_t index = 0; index < nodeCount; ++index) {
        if (covered[index] == 0) {
            FlowPath path;
            path.tracklets.push_back(nodes[index].id);
            paths.push_back(std::move(path));
        }
    }

    std::sort(paths.begin(), paths.end(), [](const FlowPath& lhs, const FlowPath& rhs) {
        return lhs.tracklets.front() < rhs.tracklets.front();
    });
    return paths;
}

} // namespace

SspSolver::SspSolver(SspSolverOptions options) : m_options(options) {}

Result<SolverResult> SspSolver::Solve(const TrackletGraph& graph) {
    const std::vector<TrackletNode>& nodes = graph.Nodes();
    const std::size_t nodeCount = nodes.size();
    if (nodeCount == 0) {
        SolverResult empty;
        empty.optimal = true;
        return empty;
    }

    const double linkReward = graph.LinkReward();
    const int source = 0;
    const int sink = static_cast<int>(2 * nodeCount + 1);
    const int totalNodes = sink + 1;
    const double epsilon = std::max(m_options.epsilon, 1e-12);

    std::vector<std::vector<ResidualEdge>> adjacency(static_cast<std::size_t>(totalNodes));
    std::vector<OriginalEdge> originals;
    originals.reserve(3 * nodeCount + graph.EdgeCount());

    const auto pre = [](std::size_t index) {
        return static_cast<int>(2 * index + 1);
    };
    const auto post = [](std::size_t index) {
        return static_cast<int>(2 * index + 2);
    };

    for (std::size_t index = 0; index < nodeCount; ++index) {
        const int edge = AddEdge(adjacency, source, pre(index), 1, 0.0);
        originals.push_back(OriginalEdge{source, pre(index), edge, static_cast<int>(index), -1,
                                         false, 0.0});
    }
    for (std::size_t index = 0; index < nodeCount; ++index) {
        const int edge = AddEdge(adjacency, pre(index), post(index), 1, 0.0);
        originals.push_back(OriginalEdge{pre(index), post(index), edge,
                                         static_cast<int>(index), static_cast<int>(index), false,
                                         0.0});
    }
    for (std::size_t index = 0; index < nodeCount; ++index) {
        const int edge = AddEdge(adjacency, post(index), sink, 1, 0.0);
        originals.push_back(OriginalEdge{post(index), sink, edge, static_cast<int>(index), -1,
                                         false, 0.0});
    }

    std::unordered_map<TrackletId, std::size_t> indexOf;
    indexOf.reserve(nodeCount);
    for (std::size_t index = 0; index < nodeCount; ++index) {
        indexOf.emplace(nodes[index].id, index);
    }
    for (const TransitionEdge& edge : graph.Edges()) {
        const auto from = indexOf.find(edge.from);
        const auto to = indexOf.find(edge.to);
        if (from == indexOf.end() || to == indexOf.end()) {
            continue;
        }
        const int adjacencyIndex = AddEdge(adjacency, post(from->second), pre(to->second), 1,
                                           edge.cost - linkReward);
        // The reported path cost keeps the raw `c_total`; only the network uses
        // the rewarded cost.
        originals.push_back(OriginalEdge{post(from->second), pre(to->second), adjacencyIndex,
                                         static_cast<int>(from->second),
                                         static_cast<int>(to->second), true, edge.cost});
    }

    // The association graph is a DAG by construction; use it for the initial
    // potentials instead of a general Bellman-Ford pass.
    std::vector<int> indegree(static_cast<std::size_t>(totalNodes), 0);
    for (const auto& list : adjacency) {
        for (const ResidualEdge& edge : list) {
            if (edge.capacity > 0) {
                ++indegree[static_cast<std::size_t>(edge.to)];
            }
        }
    }
    std::queue<int> ready;
    for (int node = 0; node < totalNodes; ++node) {
        if (indegree[static_cast<std::size_t>(node)] == 0) {
            ready.push(node);
        }
    }
    std::vector<int> topological;
    topological.reserve(static_cast<std::size_t>(totalNodes));
    while (!ready.empty()) {
        const int node = ready.front();
        ready.pop();
        topological.push_back(node);
        for (const ResidualEdge& edge : adjacency[static_cast<std::size_t>(node)]) {
            if (edge.capacity <= 0) {
                continue;
            }
            int& value = indegree[static_cast<std::size_t>(edge.to)];
            --value;
            if (value == 0) {
                ready.push(edge.to);
            }
        }
    }
    if (topological.size() != static_cast<std::size_t>(totalNodes)) {
        return Status::Error(ErrorCode::Internal,
                             "association graph contains a cycle; SspSolver requires a DAG");
    }

    std::vector<double> potential(static_cast<std::size_t>(totalNodes), 0.0);
    std::vector<double> distance(static_cast<std::size_t>(totalNodes), kInfinity);
    distance[static_cast<std::size_t>(source)] = 0.0;
    for (const int node : topological) {
        const double current = distance[static_cast<std::size_t>(node)];
        if (!std::isfinite(current)) {
            continue;
        }
        for (const ResidualEdge& edge : adjacency[static_cast<std::size_t>(node)]) {
            if (edge.capacity <= 0) {
                continue;
            }
            const double candidate = current + edge.cost;
            double& target = distance[static_cast<std::size_t>(edge.to)];
            if (candidate < target) {
                target = candidate;
            }
        }
    }
    for (int node = 0; node < totalNodes; ++node) {
        if (std::isfinite(distance[static_cast<std::size_t>(node)])) {
            potential[static_cast<std::size_t>(node)] = distance[static_cast<std::size_t>(node)];
        }
    }

    std::size_t augmentations = 0;
    std::vector<double> reduced(static_cast<std::size_t>(totalNodes), kInfinity);
    std::vector<int> parentNode(static_cast<std::size_t>(totalNodes), -1);
    std::vector<int> parentEdge(static_cast<std::size_t>(totalNodes), -1);

    using QueueItem = std::pair<double, int>;
    for (;;) {
        std::fill(reduced.begin(), reduced.end(), kInfinity);
        std::fill(parentNode.begin(), parentNode.end(), -1);
        std::fill(parentEdge.begin(), parentEdge.end(), -1);
        std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
        reduced[static_cast<std::size_t>(source)] = 0.0;
        queue.emplace(0.0, source);

        while (!queue.empty()) {
            const auto [currentDistance, node] = queue.top();
            queue.pop();
            if (currentDistance > reduced[static_cast<std::size_t>(node)] + epsilon) {
                continue;
            }
            const auto& list = adjacency[static_cast<std::size_t>(node)];
            for (std::size_t edgeIndex = 0; edgeIndex < list.size(); ++edgeIndex) {
                const ResidualEdge& edge = list[edgeIndex];
                if (edge.capacity <= 0) {
                    continue;
                }
                double reducedCost = edge.cost + potential[static_cast<std::size_t>(node)] -
                                     potential[static_cast<std::size_t>(edge.to)];
                if (reducedCost < 0.0) {
                    reducedCost = 0.0;
                }
                const double candidate = currentDistance + reducedCost;
                double& target = reduced[static_cast<std::size_t>(edge.to)];
                if (candidate + epsilon < target) {
                    target = candidate;
                    parentNode[static_cast<std::size_t>(edge.to)] = node;
                    parentEdge[static_cast<std::size_t>(edge.to)] = static_cast<int>(edgeIndex);
                    queue.emplace(candidate, edge.to);
                }
            }
        }

        if (!std::isfinite(reduced[static_cast<std::size_t>(sink)])) {
            break;
        }
        const double pathCost = reduced[static_cast<std::size_t>(sink)] +
                                potential[static_cast<std::size_t>(sink)] -
                                potential[static_cast<std::size_t>(source)];
        if (pathCost >= -epsilon) {
            break;
        }

        for (int node = 0; node < totalNodes; ++node) {
            if (std::isfinite(reduced[static_cast<std::size_t>(node)])) {
                potential[static_cast<std::size_t>(node)] +=
                    reduced[static_cast<std::size_t>(node)];
            }
        }

        int node = sink;
        while (node != source) {
            const int previous = parentNode[static_cast<std::size_t>(node)];
            if (previous < 0) {
                return Status::Error(ErrorCode::Internal, "broken augmenting path");
            }
            const int edgeIndex = parentEdge[static_cast<std::size_t>(node)];
            ResidualEdge& edge =
                adjacency[static_cast<std::size_t>(previous)]
                         [static_cast<std::size_t>(edgeIndex)];
            --edge.capacity;
            adjacency[static_cast<std::size_t>(node)]
                     [static_cast<std::size_t>(edge.reverse)]
                         .capacity += 1;
            node = previous;
        }
        ++augmentations;
    }

    SolverResult result;
    result.paths = BuildPaths(graph, adjacency, originals, nodeCount);
    for (const FlowPath& path : result.paths) {
        result.total_cost += path.cost;
    }
    result.augmentations = augmentations;
    result.optimal = true;
    return result;
}

Result<SolverResult> GreedyAssociator::Solve(const TrackletGraph& graph) {
    const std::vector<TrackletNode>& nodes = graph.Nodes();
    const std::size_t nodeCount = nodes.size();
    if (nodeCount == 0) {
        SolverResult empty;
        empty.optimal = true;
        return empty;
    }

    const double linkReward = graph.LinkReward();
    std::unordered_map<TrackletId, std::size_t> indexOf;
    indexOf.reserve(nodeCount);
    for (std::size_t index = 0; index < nodeCount; ++index) {
        indexOf.emplace(nodes[index].id, index);
    }

    struct Candidate {
        std::size_t from = 0;
        std::size_t to = 0;
        double cost = 0.0;
        double rawCost = 0.0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(graph.EdgeCount());
    for (const TransitionEdge& edge : graph.Edges()) {
        const auto from = indexOf.find(edge.from);
        const auto to = indexOf.find(edge.to);
        if (from == indexOf.end() || to == indexOf.end()) {
            continue;
        }
        const double cost = edge.cost - linkReward;
        if (cost >= 0.0) {
            continue;
        }
        candidates.push_back(Candidate{from->second, to->second, cost, edge.cost});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                  if (lhs.cost != rhs.cost) {
                      return lhs.cost < rhs.cost;
                  }
                  if (lhs.from != rhs.from) {
                      return lhs.from < rhs.from;
                  }
                  return lhs.to < rhs.to;
              });

    std::vector<int> next(nodeCount, -1);
    std::vector<int> previous(nodeCount, -1);
    std::vector<double> nextCost(nodeCount, 0.0);
    std::size_t selected = 0;
    for (const Candidate& candidate : candidates) {
        if (next[candidate.from] >= 0 || previous[candidate.to] >= 0) {
            continue;
        }
        bool cycle = false;
        std::size_t current = candidate.to;
        while (current < nodeCount) {
            if (current == candidate.from) {
                cycle = true;
                break;
            }
            const int follow = next[current];
            if (follow < 0) {
                break;
            }
            current = static_cast<std::size_t>(follow);
        }
        if (cycle) {
            continue;
        }
        next[candidate.from] = static_cast<int>(candidate.to);
        previous[candidate.to] = static_cast<int>(candidate.from);
        nextCost[candidate.from] = candidate.rawCost;
        ++selected;
    }

    std::vector<FlowPath> paths;
    std::vector<char> covered(nodeCount, 0);
    for (std::size_t index = 0; index < nodeCount; ++index) {
        if (previous[index] >= 0) {
            continue;
        }
        FlowPath path;
        std::size_t current = index;
        double cost = 0.0;
        while (current < nodeCount && covered[current] == 0) {
            covered[current] = 1;
            path.tracklets.push_back(nodes[current].id);
            const int follow = next[current];
            if (follow < 0) {
                break;
            }
            cost += nextCost[current];
            current = static_cast<std::size_t>(follow);
        }
        path.cost = cost;
        paths.push_back(std::move(path));
    }
    for (std::size_t index = 0; index < nodeCount; ++index) {
        if (covered[index] == 0) {
            FlowPath path;
            path.tracklets.push_back(nodes[index].id);
            paths.push_back(std::move(path));
        }
    }

    std::sort(paths.begin(), paths.end(), [](const FlowPath& lhs, const FlowPath& rhs) {
        return lhs.tracklets.front() < rhs.tracklets.front();
    });

    SolverResult result;
    result.paths = std::move(paths);
    for (const FlowPath& path : result.paths) {
        result.total_cost += path.cost;
    }
    result.augmentations = selected;
    result.optimal = false;
    return result;
}

Result<std::unique_ptr<IMcfSolver>> CreateMcfSolver(SolverKind kind) {
    switch (kind) {
    case SolverKind::Ssp:
        return std::unique_ptr<IMcfSolver>(std::make_unique<SspSolver>());
    case SolverKind::Greedy:
        return std::unique_ptr<IMcfSolver>(std::make_unique<GreedyAssociator>());
    case SolverKind::MuSsp:
#if defined(TMCMT_WITH_MUSSP) && TMCMT_WITH_MUSSP
        return std::unique_ptr<IMcfSolver>(std::make_unique<MuSspSolver>());
#else
        return Status::Error(ErrorCode::Unsupported,
                             "muSSP solver is not available in this build");
#endif
    }
    return Status::Error(ErrorCode::InvalidArgument, "unknown MCF solver kind");
}

} // namespace tmcmt::flow
