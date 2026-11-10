#include "trafficmcmt/flow/McfSolver.h"

#if defined(TMCMT_WITH_MUSSP) && TMCMT_WITH_MUSSP

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
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

    // muSSP returns augmenting paths in the residual graph: `pathSet` is not a
    // set of final trajectories. The final flow is the symmetric difference of
    // all augmenting paths (see `print_solution` in muSSP's main.cpp), so each
    // traversed transition edge is toggled once per occurrence. The residual
    // path may traverse an edge in reverse, hence the lookup is directionless.
    std::map<std::pair<int, int>, std::pair<std::size_t, std::size_t>> transitionByNodePair;
    for (const auto& entry : transitionCosts) {
        const int post = static_cast<int>(2 * entry.first.first + 2);
        const int pre = static_cast<int>(2 * entry.first.second + 1);
        transitionByNodePair.emplace(
            std::make_pair(std::min(post, pre), std::max(post, pre)), entry.first);
    }

    std::set<std::pair<std::size_t, std::size_t>> selected;
    for (const std::vector<int>& muPath : pathSet) {
        for (std::size_t index = 0; index + 1 < muPath.size(); ++index) {
            const int tail = muPath[index];
            const int head = muPath[index + 1];
            const auto it = transitionByNodePair.find(
                std::make_pair(std::min(tail, head), std::max(tail, head)));
            if (it == transitionByNodePair.end()) {
                continue;
            }
            const auto found = selected.find(it->second);
            if (found == selected.end()) {
                selected.insert(it->second);
            } else {
                selected.erase(found);
            }
        }
    }

    std::vector<int> nextTracklet(nodeCount, -1);
    std::vector<int> previousTracklet(nodeCount, -1);
    std::vector<double> nextCost(nodeCount, 0.0);
    for (const auto& [from, to] : selected) {
        nextTracklet[from] = static_cast<int>(to);
        previousTracklet[to] = static_cast<int>(from);
        nextCost[from] = transitionCosts.at({from, to});
    }

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
