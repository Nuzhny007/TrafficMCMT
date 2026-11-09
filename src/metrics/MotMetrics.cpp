#include "trafficmcmt/metrics/MotMetrics.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tmcmt::metrics {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::max() / 4.0;

} // namespace

std::vector<std::size_t> MotMetrics::Hungarian(
    const std::vector<std::vector<double>>& cost) {
    const std::size_t rows = cost.size();
    if (rows == 0) {
        return {};
    }
    std::size_t columns = 0;
    for (const auto& row : cost) {
        columns = std::max(columns, row.size());
    }
    if (columns == 0) {
        return std::vector<std::size_t>(rows, columns);
    }

    const std::size_t size = std::max(rows, columns);
    std::vector<std::vector<double>> matrix(size, std::vector<double>(size, 0.0));
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < cost[row].size(); ++column) {
            matrix[row][column] = cost[row][column];
        }
    }

    std::vector<double> rowPotential(size + 1, 0.0);
    std::vector<double> columnPotential(size + 1, 0.0);
    std::vector<std::size_t> matching(size + 1, 0);
    std::vector<std::size_t> way(size + 1, 0);

    for (std::size_t row = 1; row <= size; ++row) {
        matching[0] = row;
        std::size_t column = 0;
        std::vector<double> minimum(size + 1, kInfinity);
        std::vector<char> used(size + 1, 0);
        do {
            used[column] = 1;
            const std::size_t currentRow = matching[column];
            double delta = kInfinity;
            std::size_t nextColumn = 0;
            for (std::size_t candidate = 1; candidate <= size; ++candidate) {
                if (used[candidate] != 0) {
                    continue;
                }
                const double value = matrix[currentRow - 1][candidate - 1] -
                                     rowPotential[currentRow] - columnPotential[candidate];
                if (value < minimum[candidate]) {
                    minimum[candidate] = value;
                    way[candidate] = column;
                }
                if (minimum[candidate] < delta) {
                    delta = minimum[candidate];
                    nextColumn = candidate;
                }
            }
            for (std::size_t candidate = 0; candidate <= size; ++candidate) {
                if (used[candidate] != 0) {
                    rowPotential[matching[candidate]] += delta;
                    columnPotential[candidate] -= delta;
                } else {
                    minimum[candidate] -= delta;
                }
            }
            column = nextColumn;
        } while (matching[column] != 0);

        do {
            const std::size_t previousColumn = way[column];
            matching[column] = matching[previousColumn];
            column = previousColumn;
        } while (column != 0);
    }

    std::vector<std::size_t> assignment(rows, columns);
    for (std::size_t column = 1; column <= size; ++column) {
        const std::size_t row = matching[column];
        if (row == 0 || row - 1 >= rows || column - 1 >= columns) {
            continue;
        }
        assignment[row - 1] = column - 1;
    }
    return assignment;
}

MetricsReport MotMetrics::Compute(std::span<const TrackIdentity> gt,
                                  std::span<const PredictedIdentity> predicted) {
    MetricsReport report;
    report.gt_tracks = gt.size();
    report.predicted_tracks = predicted.size();

    std::unordered_map<TrackletId, GlobalId> predictedById;
    predictedById.reserve(predicted.size());
    for (const PredictedIdentity& entry : predicted) {
        predictedById.insert_or_assign(entry.tracklet_id, entry.global_id);
    }

    std::set<std::string> gtIdentitySet;
    for (const TrackIdentity& entry : gt) {
        gtIdentitySet.insert(entry.identity);
    }
    std::set<GlobalId> predictedIdSet;
    for (const PredictedIdentity& entry : predicted) {
        if (entry.global_id != kInvalidGlobalId) {
            predictedIdSet.insert(entry.global_id);
        }
    }

    const std::vector<std::string> gtIdentities(gtIdentitySet.begin(), gtIdentitySet.end());
    const std::vector<GlobalId> predictedIds(predictedIdSet.begin(), predictedIdSet.end());
    std::unordered_map<std::string, std::size_t> gtIndex;
    for (std::size_t index = 0; index < gtIdentities.size(); ++index) {
        gtIndex.emplace(gtIdentities[index], index);
    }
    std::unordered_map<GlobalId, std::size_t> predictedIndex;
    for (std::size_t index = 0; index < predictedIds.size(); ++index) {
        predictedIndex.emplace(predictedIds[index], index);
    }

    std::vector<std::vector<std::size_t>> counts(
        gtIdentities.size(), std::vector<std::size_t>(predictedIds.size(), 0));
    std::vector<std::optional<GlobalId>> gtPrediction(gt.size());
    for (std::size_t index = 0; index < gt.size(); ++index) {
        const auto prediction = predictedById.find(gt[index].tracklet_id);
        if (prediction == predictedById.end() || prediction->second == kInvalidGlobalId) {
            continue;
        }
        gtPrediction[index] = prediction->second;
        const auto gtIt = gtIndex.find(gt[index].identity);
        const auto predictedIt = predictedIndex.find(prediction->second);
        if (gtIt != gtIndex.end() && predictedIt != predictedIndex.end()) {
            ++counts[gtIt->second][predictedIt->second];
        }
    }

    std::size_t maximumCount = 0;
    for (const auto& row : counts) {
        for (const std::size_t value : row) {
            maximumCount = std::max(maximumCount, value);
        }
    }
    std::vector<std::vector<double>> costMatrix(
        gtIdentities.size(), std::vector<double>(predictedIds.size(), 0.0));
    for (std::size_t row = 0; row < counts.size(); ++row) {
        for (std::size_t column = 0; column < counts[row].size(); ++column) {
            costMatrix[row][column] =
                static_cast<double>(maximumCount - counts[row][column]);
        }
    }
    const std::vector<std::size_t> assignment = Hungarian(costMatrix);
    std::size_t idtp = 0;
    for (std::size_t row = 0; row < assignment.size(); ++row) {
        if (assignment[row] < predictedIds.size()) {
            idtp += counts[row][assignment[row]];
        }
    }
    report.idtp = idtp;
    report.idfp = report.predicted_tracks >= idtp ? report.predicted_tracks - idtp : 0;
    report.idfn = report.gt_tracks >= idtp ? report.gt_tracks - idtp : 0;

    std::unordered_map<std::string, std::vector<std::size_t>> byIdentity;
    for (std::size_t index = 0; index < gt.size(); ++index) {
        byIdentity[gt[index].identity].push_back(index);
    }
    for (auto& [identity, indices] : byIdentity) {
        static_cast<void>(identity);
        std::sort(indices.begin(), indices.end(),
                  [&gt](std::size_t lhs, std::size_t rhs) {
                      if (gt[lhs].begin != gt[rhs].begin) {
                          return gt[lhs].begin < gt[rhs].begin;
                      }
                      return gt[lhs].tracklet_id < gt[rhs].tracklet_id;
                  });
        GlobalId previous = kInvalidGlobalId;
        for (const std::size_t index : indices) {
            if (!gtPrediction[index].has_value()) {
                continue;
            }
            const GlobalId current = *gtPrediction[index];
            if (previous != kInvalidGlobalId && current != previous) {
                ++report.idsw;
            }
            previous = current;
        }
    }

    const auto ratio = [](double numerator, double denominator) {
        return denominator > 0.0 ? numerator / denominator : 0.0;
    };
    const double tp = static_cast<double>(report.idtp);
    const double fp = static_cast<double>(report.idfp);
    const double fn = static_cast<double>(report.idfn);
    report.idp = ratio(tp, tp + fp);
    report.idr = ratio(tp, tp + fn);
    report.idf1 = ratio(2.0 * tp, 2.0 * tp + fp + fn);
    return report;
}

} // namespace tmcmt::metrics
