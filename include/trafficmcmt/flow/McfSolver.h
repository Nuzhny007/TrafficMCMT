#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/flow/TrackletGraph.h"

namespace tmcmt::flow {

/// One global trajectory: a chain of tracklets in chronological order.
struct FlowPath {
    std::vector<TrackletId> tracklets;
    /// Sum of the raw transition costs of the chain.
    double cost = 0.0;
};

struct SolverResult {
    /// Every input node appears in exactly one path. Nodes without a selected
    /// transition are returned as singleton paths.
    std::vector<FlowPath> paths;
    double total_cost = 0.0;
    /// Number of augmenting paths selected by the solver (0 for greedy).
    std::size_t augmentations = 0;
    /// True when the solver returns the optimal path cover.
    bool optimal = false;
};

class IMcfSolver {
public:
    virtual ~IMcfSolver() = default;

    [[nodiscard]] virtual Result<SolverResult> Solve(const TrackletGraph& graph) = 0;
    [[nodiscard]] virtual const char* Name() const noexcept = 0;
};

struct SspSolverOptions {
    /// Paths whose cost is within this tolerance of zero are not selected.
    double epsilon = 1e-9;
};

/// Successive shortest path solver over the split-node network.
///
/// The implementation is exact for the path-cover objective and is the
/// reference/fallback solver when muSSP is not available. It only augments
/// while the shortest source-to-sink path has a strictly negative cost.
class SspSolver final : public IMcfSolver {
public:
    explicit SspSolver(SspSolverOptions options = {});

    [[nodiscard]] Result<SolverResult> Solve(const TrackletGraph& graph) override;
    [[nodiscard]] const char* Name() const noexcept override {
        return "ssp";
    }

private:
    SspSolverOptions m_options;
};

/// Greedy baseline: transitions are accepted in ascending cost order while
/// both endpoints are still free. Never claimed to be optimal.
class GreedyAssociator final : public IMcfSolver {
public:
    GreedyAssociator() = default;

    [[nodiscard]] Result<SolverResult> Solve(const TrackletGraph& graph) override;
    [[nodiscard]] const char* Name() const noexcept override {
        return "greedy";
    }
};

#if defined(TMCMT_WITH_MUSSP) && TMCMT_WITH_MUSSP
/// Adapter for the muSSP min-cost flow implementation.
class MuSspSolver final : public IMcfSolver {
public:
    MuSspSolver() = default;

    [[nodiscard]] Result<SolverResult> Solve(const TrackletGraph& graph) override;
    [[nodiscard]] const char* Name() const noexcept override {
        return "mussp";
    }
};
#endif

/// Creates a solver for the requested kind. `MuSsp` returns `Unsupported`
/// when the library was built without muSSP.
[[nodiscard]] Result<std::unique_ptr<IMcfSolver>> CreateMcfSolver(SolverKind kind);

} // namespace tmcmt::flow
