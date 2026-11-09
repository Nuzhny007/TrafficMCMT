#include "trafficmcmt/flow/McfSolver.h"

#if defined(TMCMT_WITH_MUSSP) && TMCMT_WITH_MUSSP

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Graph.h"

namespace tmcmt::flow {
namespace {

class StreamRedirect {
public:
    explicit StreamRedirect(std::ostream& stream) : m_stream(stream), m_old(stream.rdbuf()) {
        m_stream.rdbuf(m_buffer.rdbuf());
    }

    ~StreamRedirect() {
        m_stream.rdbuf(m_old);
    }

    StreamRedirect(const StreamRedirect&) = delete;
    StreamRedirect& operator=(const StreamRedirect&) = delete;

private:
    std::ostream& m_stream;
    std::streambuf* m_old;
    std::ostringstream m_buffer;
};

} // namespace

Result<SolverResult> MuSspSolver::Solve(const TrackletGraph& graph) {
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

    std::map<std::pair<std::size_t, std::size_t>, double> transitionCosts;
    for (const TransitionEdge& edge : graph.Edges()) {
        const auto from = indexOf.find(edge.from);
        const auto to = indexOf.find(edge.to);
        if (from == indexOf.end() || to == indexOf.end()) {
            continue;
        }
        const auto key = std::make_pair(from->second, to->second);
        const auto existing = transitionCosts.find(key);
        if (existing == transitionCosts.end() || edge.cost < existing->second) {
            transitionCosts.insert_or_assign(key, edge.cost);
        }
    }

    const int totalNodes = static_cast<int>(2 * nodeCount + 2);
    const int sink = totalNodes - 1;
    const int totalEdges = static_cast<int>(3 * nodeCount + transitionCosts.size());
    auto muGraph = std::make_unique<Graph>(totalNodes, totalEdges, 0, sink, 0.0, 0.0);

    int edgeId = 0;
    for (std::size_t index = 0; index < nodeCount; ++index) {
        muGraph->add_edge(0, static_cast<int>(2 * index + 1), edgeId++, 0.0);
    }
    for (std::size_t index = 0; index < nodeCount; ++index) {
        muGraph->add_edge(static_cast<int>(2 * index + 1),
                          static_cast<int>(2 * index + 2), edgeId++, 0.0);
    }
    for (std::size_t index = 0; index < nodeCount; ++index) {
        muGraph->add_edge(static_cast<int>(2 * index + 2), sink, edgeId++, 0.0);
    }
    for (const auto& [key, cost] : transitionCosts) {
        muGraph->add_edge(static_cast<int>(2 * key.first + 2),
                          static_cast<int>(2 * key.second + 1), edgeId++, cost - linkReward);
    }

    std::vector<std::vector<int>> pathSet;
    {
        StreamRedirect redirect(std::cout);

        muGraph->invalid_edge_rm();
        muGraph->shortest_path_dag();

        std::vector<double> pathCosts;
        pathCosts.push_back(muGraph->distance2src[static_cast<std::size_t>(muGraph->sink_id_)]);
        muGraph->cur_path_max_cost = -muGraph->distance2src[static_cast<std::size_t>(
            muGraph->sink_id_)];

        muGraph->update_allgraph_weights();
        muGraph->extract_shortest_path();
        pathSet.push_back(muGraph->shortest_path);

        std::vector<int> updateNodes;
        muGraph->find_node_set4update(updateNodes);
        muGraph->flip_path();

        for (;;) {
            muGraph->update_shortest_path_tree_recursive(updateNodes);
            muGraph->update_sink_info(updateNodes);
            muGraph->extract_shortest_path();

            const double current =
                pathCosts.back() +
                muGraph->distance2src[static_cast<std::size_t>(muGraph->sink_id_)];
            if (current > -0.0000001) {
                break;
            }
            pathCosts.push_back(current);
            muGraph->cur_path_max_cost = -current;
            pathSet.push_back(muGraph->shortest_path);

            muGraph->update_subgraph_weights(updateNodes);
            muGraph->find_node_set4update(updateNodes);
            muGraph->flip_path();
        }
    }

    std::vector<FlowPath> paths;
    std::vector<char> covered(nodeCount, 0);
    for (const std::vector<int>& muPath : pathSet) {
        std::vector<std::size_t> sequence;
        for (const int node : muPath) {
            if (node >= 2 && node <= static_cast<int>(2 * nodeCount) && node % 2 == 0) {
                sequence.push_back(static_cast<std::size_t>((node - 2) / 2));
            }
        }
        std::reverse(sequence.begin(), sequence.end());
        if (sequence.empty()) {
            continue;
        }

        FlowPath path;
        double cost = 0.0;
        for (std::size_t index = 0; index < sequence.size(); ++index) {
            path.tracklets.push_back(nodes[sequence[index]].id);
            if (index > 0) {
                const auto it =
                    transitionCosts.find(std::make_pair(sequence[index - 1], sequence[index]));
                if (it != transitionCosts.end()) {
                    cost += it->second;
                }
            }
            covered[sequence[index]] = 1;
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
    result.augmentations = pathSet.size();
    result.optimal = true;
    return result;
}

} // namespace tmcmt::flow

#endif // TMCMT_WITH_MUSSP
