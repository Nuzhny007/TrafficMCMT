#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/flow/FlowAssociator.h"
#include "trafficmcmt/flow/GlobalIdManager.h"
#include "trafficmcmt/flow/GraphPruner.h"
#include "trafficmcmt/flow/McfSolver.h"
#include "trafficmcmt/flow/TrackletGraph.h"

namespace {

using namespace tmcmt;
using namespace tmcmt::flow;

Timestamp Ms(std::int64_t value) {
    return Timestamp(value);
}

TrackletNode Node(TrackletId id, std::int64_t begin, std::int64_t end) {
    TrackletNode node;
    node.id = id;
    node.camera = "cam-1";
    node.begin = Ms(begin);
    node.end = Ms(end);
    node.type = ObjectType::Car;
    node.quality = 1.f;
    return node;
}

Tracklet MakeTracklet(TrackletId id, const CameraId& camera, std::int64_t begin,
                      std::int64_t end) {
    Tracklet tracklet;
    tracklet.id = id;
    tracklet.session_id = "session";
    tracklet.camera = camera;
    tracklet.local_track_id = id;
    tracklet.begin = Ms(begin);
    tracklet.end = Ms(end);
    tracklet.type = ObjectType::Car;
    tracklet.quality = 1.f;
    return tracklet;
}

double BestCoverBenefit(const std::vector<std::vector<double>>& benefit) {
    const std::size_t count = benefit.size();
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (std::size_t from = 0; from < count; ++from) {
        for (std::size_t to = 0; to < count; ++to) {
            if (from != to && benefit[from][to] > 0.0) {
                edges.emplace_back(from, to);
            }
        }
    }

    double best = 0.0;
    const std::size_t subsets = std::size_t{1} << edges.size();
    for (std::size_t mask = 0; mask < subsets; ++mask) {
        std::vector<char> usedOut(count, 0);
        std::vector<char> usedIn(count, 0);
        double sum = 0.0;
        bool valid = true;
        for (std::size_t index = 0; index < edges.size(); ++index) {
            if ((mask >> index) % 2 == 0) {
                continue;
            }
            const auto [from, to] = edges[index];
            if (usedOut[from] != 0 || usedIn[to] != 0) {
                valid = false;
                break;
            }
            usedOut[from] = 1;
            usedIn[to] = 1;
            sum += benefit[from][to];
        }
        if (valid) {
            best = std::max(best, sum);
        }
    }
    return best;
}

void ExpectValidCover(const TrackletGraph& graph, const SolverResult& result) {
    std::set<TrackletId> covered;
    for (const FlowPath& path : result.paths) {
        EXPECT_FALSE(path.tracklets.empty());
        for (const TrackletId id : path.tracklets) {
            EXPECT_TRUE(covered.insert(id).second) << "tracklet " << id << " appears twice";
        }
    }
    EXPECT_EQ(covered.size(), graph.NodeCount());
}

TEST(TrackletGraphTest, RejectsInvalidNodesAndTransitions) {
    TrackletGraph graph;
    EXPECT_TRUE(graph.AddNode(Node(1, 0, 100)).ok());
    EXPECT_TRUE(graph.AddNode(Node(2, 150, 250)).ok());
    EXPECT_FALSE(graph.AddNode(Node(1, 0, 100)).ok());
    EXPECT_TRUE(graph.AddTransition(1, 2, 0.2).ok());
    EXPECT_FALSE(graph.AddTransition(2, 1, 0.2).ok());
    EXPECT_FALSE(graph.AddTransition(1, 1, 0.2).ok());
    EXPECT_FALSE(graph.AddTransition(1, 99, 0.2).ok());
    EXPECT_FALSE(graph.AddTransition(1, 2, std::numeric_limits<double>::quiet_NaN()).ok());
    EXPECT_EQ(graph.NodeCount(), 2u);
    EXPECT_EQ(graph.EdgeCount(), 1u);
    ASSERT_NE(graph.FindNode(1), nullptr);
    EXPECT_EQ(graph.FindNode(1)->camera, "cam-1");
    EXPECT_FALSE(graph.IndexOf(99).has_value());
}

TEST(GraphPrunerTest, DeduplicatesAndKeepsTopK) {
    TrackletGraph graph;
    for (TrackletId id = 1; id <= 4; ++id) {
        static_cast<void>(graph.AddNode(Node(id, static_cast<std::int64_t>(id * 100),
                                            static_cast<std::int64_t>(id * 100 + 50))));
    }
    static_cast<void>(graph.AddTransition(1, 2, 0.5));
    static_cast<void>(graph.AddTransition(1, 2, 0.2));
    static_cast<void>(graph.AddTransition(1, 3, 0.3));
    static_cast<void>(graph.AddTransition(1, 4, 0.1));

    GraphPruneOptions options;
    options.max_candidates_per_node = 2;
    GraphPruneStats stats;
    const GraphPruner pruner(options);
    const TrackletGraph pruned = pruner.Prune(graph, &stats);

    ASSERT_EQ(pruned.EdgeCount(), 2u);
    EXPECT_EQ(pruned.Edges()[0].to, 4u);
    EXPECT_DOUBLE_EQ(pruned.Edges()[0].cost, 0.1);
    EXPECT_EQ(pruned.Edges()[1].to, 2u);
    EXPECT_DOUBLE_EQ(pruned.Edges()[1].cost, 0.2);
    EXPECT_EQ(stats.duplicate_edges, 1u);
    EXPECT_EQ(stats.truncated_edges, 1u);
    EXPECT_EQ(stats.output_nodes, 4u);
    EXPECT_EQ(stats.output_edges, 2u);
}

TEST(GraphPrunerTest, SplitsComponentsAndSkipsLargeOnes) {
    TrackletGraph graph;
    for (TrackletId id = 1; id <= 5; ++id) {
        static_cast<void>(graph.AddNode(Node(id, static_cast<std::int64_t>(id * 100),
                                            static_cast<std::int64_t>(id * 100 + 50))));
    }
    static_cast<void>(graph.AddTransition(1, 2, 0.2));
    static_cast<void>(graph.AddTransition(3, 4, 0.2));
    static_cast<void>(graph.AddTransition(4, 5, 0.2));

    GraphPruneOptions options;
    options.max_candidates_per_node = 15;
    options.max_component_nodes = 2;
    GraphPruneStats stats;
    const GraphPruner pruner(options);
    const std::vector<TrackletGraph> components = pruner.Split(graph, &stats);

    ASSERT_EQ(components.size(), 1u);
    EXPECT_EQ(components[0].NodeCount(), 2u);
    EXPECT_EQ(components[0].EdgeCount(), 1u);
    EXPECT_EQ(stats.components, 2u);
    EXPECT_EQ(stats.skipped_components, 1u);
    EXPECT_EQ(stats.skipped_nodes, 3u);

    options.max_component_nodes = 0;
    const GraphPruner unlimited(options);
    const std::vector<TrackletGraph> all = unlimited.Split(graph);
    EXPECT_EQ(all.size(), 2u);
}

TEST(SspSolverTest, MatchesBruteForceOnSmallGraphs) {
    const std::vector<TrackletId> ids{1, 2, 3, 4, 5};
    const std::vector<std::vector<double>> costs{
        {0.0, -0.8, 0.4, -0.2, 0.3},
        {0.0, 0.0, -0.7, 0.5, -0.1},
        {0.0, 0.0, 0.0, -0.6, 0.2},
        {0.0, 0.0, 0.0, 0.0, -0.9},
        {0.0, 0.0, 0.0, 0.0, 0.0},
    };

    TrackletGraph graph;
    graph.SetLinkReward(0.0);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        static_cast<void>(graph.AddNode(
            Node(ids[index], static_cast<std::int64_t>(index * 100),
                 static_cast<std::int64_t>(index * 100 + 50))));
    }
    std::vector<std::vector<double>> benefit(ids.size(),
                                             std::vector<double>(ids.size(), 0.0));
    for (std::size_t from = 0; from < ids.size(); ++from) {
        for (std::size_t to = from + 1; to < ids.size(); ++to) {
            static_cast<void>(graph.AddTransition(ids[from], ids[to], costs[from][to]));
            benefit[from][to] = std::max(0.0, -costs[from][to]);
        }
    }

    const double optimalBenefit = BestCoverBenefit(benefit);
    SspSolver solver;
    auto result = solver.Solve(graph);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().optimal);
    EXPECT_NEAR(-result.value().total_cost, optimalBenefit, 1e-9);
    ExpectValidCover(graph, result.value());

    GreedyAssociator greedy;
    auto greedyResult = greedy.Solve(graph);
    ASSERT_TRUE(greedyResult);
    EXPECT_FALSE(greedyResult.value().optimal);
    EXPECT_LE(-greedyResult.value().total_cost, optimalBenefit + 1e-9);
    ExpectValidCover(graph, greedyResult.value());
}

TEST(SspSolverTest, MatchesBruteForceOnSparseMediumGraph) {
    constexpr std::size_t kNodes = 10;
    const std::vector<std::pair<std::size_t, std::size_t>> endpoints{
        {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 4}, {2, 3}, {2, 5},
        {3, 4}, {3, 6}, {4, 5}, {4, 7}, {5, 6}, {5, 8}, {6, 7},
        {6, 9}, {7, 8}, {8, 9}, {1, 6}, {2, 7}, {3, 8}, {0, 9},
    };
    const std::vector<double> edgeCosts{
        -0.9, 0.4, -0.2, -0.7, 0.3, -0.6, -0.1, -0.8, 0.2, -0.5, -0.3,
        -0.4, 0.1, -0.7, -0.2, -0.6, -0.9, -0.15, 0.25, -0.35, 0.5,
    };
    ASSERT_EQ(endpoints.size(), edgeCosts.size());

    TrackletGraph graph;
    graph.SetLinkReward(0.0);
    for (std::size_t index = 0; index < kNodes; ++index) {
        static_cast<void>(graph.AddNode(
            Node(static_cast<TrackletId>(index + 1),
                 static_cast<std::int64_t>(index * 100),
                 static_cast<std::int64_t>(index * 100 + 50))));
    }
    std::vector<std::vector<double>> benefit(kNodes, std::vector<double>(kNodes, 0.0));
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        const auto [from, to] = endpoints[index];
        static_cast<void>(graph.AddTransition(static_cast<TrackletId>(from + 1),
                                              static_cast<TrackletId>(to + 1),
                                              edgeCosts[index]));
        benefit[from][to] = std::max(0.0, -edgeCosts[index]);
    }

    const double optimalBenefit = BestCoverBenefit(benefit);
    SspSolver solver;
    auto result = solver.Solve(graph);
    ASSERT_TRUE(result);
    EXPECT_NEAR(-result.value().total_cost, optimalBenefit, 1e-9);
    ExpectValidCover(graph, result.value());

#ifdef TMCMT_WITH_MUSSP
    MuSspSolver muSolver;
    auto muResult = muSolver.Solve(graph);
    ASSERT_TRUE(muResult);
    EXPECT_NEAR(-muResult.value().total_cost, optimalBenefit, 1e-9);
    ExpectValidCover(graph, muResult.value());
#endif
}

TEST(SspSolverTest, MatchesBruteForceWithShuffledNodeOrder) {
    constexpr std::size_t kNodes = 10;
    // Node ids are inserted in ascending order, but their timestamps follow
    // `idForRank`, so the graph node vector is not topologically ordered.
    const std::vector<TrackletId> idForRank{5, 2, 9, 1, 7, 4, 10, 3, 8, 6};
    const std::vector<std::pair<std::size_t, std::size_t>> endpoints{
        {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 4}, {2, 3}, {2, 5},
        {3, 4}, {3, 6}, {4, 5}, {4, 7}, {5, 6}, {5, 8}, {6, 7},
        {6, 9}, {7, 8}, {8, 9}, {1, 6}, {2, 7}, {3, 8}, {0, 9},
    };
    const std::vector<double> edgeCosts{
        -0.9, 0.4, -0.2, -0.7, 0.3, -0.6, -0.1, -0.8, 0.2, -0.5, -0.3,
        -0.4, 0.1, -0.7, -0.2, -0.6, -0.9, -0.15, 0.25, -0.35, 0.5,
    };

    TrackletGraph graph;
    graph.SetLinkReward(0.0);
    for (TrackletId id = 1; id <= kNodes; ++id) {
        std::size_t rank = 0;
        while (idForRank[rank] != id) {
            ++rank;
        }
        static_cast<void>(graph.AddNode(
            Node(id, static_cast<std::int64_t>(rank * 100),
                 static_cast<std::int64_t>(rank * 100 + 50))));
    }
    std::vector<std::vector<double>> benefit(kNodes, std::vector<double>(kNodes, 0.0));
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        const auto [from, to] = endpoints[index];
        static_cast<void>(graph.AddTransition(idForRank[from], idForRank[to],
                                              edgeCosts[index]));
        benefit[from][to] = std::max(0.0, -edgeCosts[index]);
    }

    const double optimalBenefit = BestCoverBenefit(benefit);
    SspSolver solver;
    auto result = solver.Solve(graph);
    ASSERT_TRUE(result);
    EXPECT_NEAR(-result.value().total_cost, optimalBenefit, 1e-9);
    ExpectValidCover(graph, result.value());

#ifdef TMCMT_WITH_MUSSP
    MuSspSolver muSolver;
    auto muResult = muSolver.Solve(graph);
    ASSERT_TRUE(muResult);
    EXPECT_NEAR(-muResult.value().total_cost, optimalBenefit, 1e-9);
    ExpectValidCover(graph, muResult.value());
#endif
}

TEST(SspSolverTest, HandlesEmptyGraphAndFactory) {
    TrackletGraph empty;
    SspSolver solver;
    auto result = solver.Solve(empty);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().paths.empty());
    EXPECT_TRUE(result.value().optimal);

    auto ssp = CreateMcfSolver(SolverKind::Ssp);
    ASSERT_TRUE(ssp);
    EXPECT_STREQ(ssp.value()->Name(), "ssp");

    auto greedy = CreateMcfSolver(SolverKind::Greedy);
    ASSERT_TRUE(greedy);
    EXPECT_STREQ(greedy.value()->Name(), "greedy");

    auto mussp = CreateMcfSolver(SolverKind::MuSsp);
#ifdef TMCMT_WITH_MUSSP
    ASSERT_TRUE(mussp);
    EXPECT_STREQ(mussp.value()->Name(), "mussp");
#else
    EXPECT_FALSE(mussp);
    EXPECT_EQ(mussp.status().code(), ErrorCode::Unsupported);
#endif
}

TEST(MuSspSolverTest, MatchesBruteForceWhenAvailable) {
#ifdef TMCMT_WITH_MUSSP
    const std::vector<TrackletId> ids{1, 2, 3, 4, 5};
    const std::vector<std::vector<double>> costs{
        {0.0, -0.8, 0.4, -0.2, 0.3},
        {0.0, 0.0, -0.7, 0.5, -0.1},
        {0.0, 0.0, 0.0, -0.6, 0.2},
        {0.0, 0.0, 0.0, 0.0, -0.9},
        {0.0, 0.0, 0.0, 0.0, 0.0},
    };
    TrackletGraph graph;
    graph.SetLinkReward(0.0);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        static_cast<void>(graph.AddNode(
            Node(ids[index], static_cast<std::int64_t>(index * 100),
                 static_cast<std::int64_t>(index * 100 + 50))));
    }
    std::vector<std::vector<double>> benefit(ids.size(),
                                             std::vector<double>(ids.size(), 0.0));
    for (std::size_t from = 0; from < ids.size(); ++from) {
        for (std::size_t to = from + 1; to < ids.size(); ++to) {
            static_cast<void>(graph.AddTransition(ids[from], ids[to], costs[from][to]));
            benefit[from][to] = std::max(0.0, -costs[from][to]);
        }
    }
    const double optimalBenefit = BestCoverBenefit(benefit);
    MuSspSolver solver;
    auto result = solver.Solve(graph);
    ASSERT_TRUE(result);
    EXPECT_NEAR(-result.value().total_cost, optimalBenefit, 1e-9);
    ExpectValidCover(graph, result.value());
#else
    GTEST_SKIP() << "built without muSSP";
#endif
}

TEST(FlowAssociatorTest, BuildsOneGlobalTrajectory) {
    std::vector<Tracklet> tracklets{
        MakeTracklet(1, "cam-1", 0, 100),
        MakeTracklet(2, "cam-2", 150, 250),
        MakeTracklet(3, "cam-1", 300, 400),
        MakeTracklet(4, "cam-2", 450, 550),
        MakeTracklet(5, "cam-1", 1000, 1100),
    };
    std::vector<TransitionEdge> transitions{
        TransitionEdge{1, 2, 0.2},
        TransitionEdge{2, 3, 0.3},
        TransitionEdge{3, 4, 0.1},
        TransitionEdge{1, 3, 0.8},
        TransitionEdge{2, 1, 0.4},
    };

    FlowAssociationOptions options;
    options.solver = SolverKind::Ssp;
    const FlowAssociator associator(options);
    auto result = associator.Associate(tracklets, transitions);
    ASSERT_TRUE(result) << result.status().ToString();

    const FlowAssociationResult& association = result.value();
    EXPECT_EQ(association.stats.tracklets, 5u);
    EXPECT_EQ(association.stats.transitions, 5u);
    EXPECT_EQ(association.stats.rejected_transitions, 1u);
    EXPECT_EQ(association.paths.size(), 2u);
    EXPECT_EQ(association.assignments.size(), 5u);
    EXPECT_TRUE(association.optimal);

    const auto first = association.assignments.find(1);
    const auto second = association.assignments.find(2);
    const auto third = association.assignments.find(3);
    const auto fourth = association.assignments.find(4);
    const auto isolated = association.assignments.find(5);
    ASSERT_NE(first, association.assignments.end());
    ASSERT_NE(second, association.assignments.end());
    ASSERT_NE(third, association.assignments.end());
    ASSERT_NE(fourth, association.assignments.end());
    ASSERT_NE(isolated, association.assignments.end());
    EXPECT_EQ(first->second, second->second);
    EXPECT_EQ(first->second, third->second);
    EXPECT_EQ(first->second, fourth->second);
    EXPECT_NE(first->second, isolated->second);
    EXPECT_NE(first->second, kInvalidGlobalId);
}

TEST(FlowAssociatorTest, SkipsOversizedComponents) {
    std::vector<Tracklet> tracklets{
        MakeTracklet(1, "cam-1", 0, 100),
        MakeTracklet(2, "cam-2", 150, 250),
        MakeTracklet(3, "cam-1", 300, 400),
        MakeTracklet(4, "cam-1", 1000, 1100),
        MakeTracklet(5, "cam-2", 1150, 1250),
    };
    std::vector<TransitionEdge> transitions{
        TransitionEdge{1, 2, 0.2},
        TransitionEdge{2, 3, 0.2},
        TransitionEdge{4, 5, 0.2},
    };
    FlowAssociationOptions options;
    options.max_component_nodes = 2;
    const FlowAssociator associator(options);
    auto result = associator.Associate(tracklets, transitions);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().stats.skipped_components, 1u);
    EXPECT_FALSE(result.value().optimal);
    EXPECT_EQ(result.value().paths.size(), 4u);
}

TEST(GlobalIdManagerTest, MergesCommitsAndNeverReusesIds) {
    GlobalIdManagerOptions options;
    options.commit_lag = Ms(500);
    GlobalIdManager manager(options);

    auto first = manager.Add(1, Ms(100));
    auto second = manager.Add(2, Ms(200));
    auto third = manager.Add(3, Ms(300));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);
    EXPECT_EQ(first.value(), 1u);
    EXPECT_EQ(second.value(), 2u);
    EXPECT_EQ(third.value(), 3u);

    ASSERT_TRUE(manager.Link(1, 2, Ms(150), "first").ok());
    EXPECT_EQ(manager.IdFor(1), 1u);
    EXPECT_EQ(manager.IdFor(2), 1u);
    ASSERT_EQ(manager.MergeEvents().size(), 1u);
    EXPECT_EQ(manager.MergeEvents()[0].old_id, 2u);
    EXPECT_EQ(manager.MergeEvents()[0].new_id, 1u);

    ASSERT_TRUE(manager.Link(2, 3, Ms(250), "second").ok());
    EXPECT_EQ(manager.IdFor(3), 1u);
    ASSERT_EQ(manager.MergeEvents().size(), 2u);
    EXPECT_EQ(manager.MergeEvents()[1].old_id, 3u);
    EXPECT_EQ(manager.MergeEvents()[1].new_id, 1u);

    ASSERT_TRUE(manager.Link(1, 3, Ms(260), "repeat").ok());
    EXPECT_EQ(manager.MergeEvents().size(), 2u);

    ASSERT_TRUE(manager.Commit(Ms(1000)).ok());
    EXPECT_TRUE(manager.IsCommitted(1));
    EXPECT_EQ(manager.GroupCount(), 1u);

    auto fourth = manager.Add(4, Ms(1000));
    ASSERT_TRUE(fourth);
    EXPECT_EQ(fourth.value(), 4u);
    ASSERT_TRUE(manager.Link(4, 1, Ms(1000), "late").ok());
    EXPECT_EQ(manager.IdFor(4), 1u);
    EXPECT_FALSE(manager.IsCommitted(4));
    ASSERT_EQ(manager.MergeEvents().size(), 3u);
    EXPECT_EQ(manager.MergeEvents()[2].old_id, 4u);
    EXPECT_EQ(manager.MergeEvents()[2].new_id, 1u);

    auto fifth = manager.Add(5, Ms(2000));
    ASSERT_TRUE(fifth);
    EXPECT_EQ(fifth.value(), 5u);
    EXPECT_EQ(manager.GroupCount(), 2u);

    const auto assignments = manager.Assignments();
    ASSERT_EQ(assignments.size(), 5u);
    EXPECT_EQ(assignments[0].first, 1u);
    EXPECT_EQ(assignments[0].second, 1u);
    EXPECT_EQ(assignments[3].first, 4u);
    EXPECT_EQ(assignments[3].second, 1u);
    EXPECT_EQ(assignments[4].first, 5u);
    EXPECT_EQ(assignments[4].second, 5u);
}

TEST(GlobalIdManagerTest, RespectsCommitLag) {
    GlobalIdManagerOptions options;
    options.commit_lag = Ms(100);
    GlobalIdManager manager(options);
    ASSERT_TRUE(manager.Add(1, Ms(100)));

    ASSERT_TRUE(manager.Commit(Ms(150)).ok());
    EXPECT_FALSE(manager.IsCommitted(1));
    ASSERT_TRUE(manager.Commit(Ms(250)).ok());
    EXPECT_TRUE(manager.IsCommitted(1));

    ASSERT_TRUE(manager.Add(1, Ms(400)));
    EXPECT_EQ(manager.IdFor(1), 1u);
}

} // namespace
