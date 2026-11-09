#include "trafficmcmt/st/PairCandidateGenerator.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

#include "trafficmcmt/reid/ReIdMatcher.h"

namespace tmcmt::st {
namespace {

void CountGate(PairGenerationStats& stats, GateReason reason) {
    switch (reason) {
    case GateReason::Allowed:
        break;
    case GateReason::SameCamera:
        ++stats.rejected_same_camera;
        break;
    case GateReason::Unlinked:
        ++stats.rejected_unlinked;
        break;
    case GateReason::OutsideWindow:
        ++stats.rejected_window;
        break;
    case GateReason::OutsideSigma:
        ++stats.rejected_sigma;
        break;
    case GateReason::MissingWorld:
        ++stats.rejected_missing_world;
        break;
    case GateReason::MissingZone:
        ++stats.rejected_missing_zone;
        break;
    case GateReason::ExitZone:
        ++stats.rejected_exit_zone;
        break;
    case GateReason::EntryZone:
        ++stats.rejected_entry_zone;
        break;
    case GateReason::Speed:
        ++stats.rejected_speed;
        break;
    case GateReason::Distance:
        ++stats.rejected_distance;
        break;
    }
}

bool Better(const PairCandidate& lhs, const PairCandidate& rhs) {
    if (lhs.costs.total != rhs.costs.total) {
        return lhs.costs.total < rhs.costs.total;
    }
    if (lhs.similarity != rhs.similarity) {
        return lhs.similarity > rhs.similarity;
    }
    if (lhs.query_id != rhs.query_id) {
        return lhs.query_id < rhs.query_id;
    }
    return lhs.candidate_id < rhs.candidate_id;
}

} // namespace

PairCandidateGenerator::PairCandidateGenerator(const reid::GalleryManager& gallery,
                                               const CameraLinkModel* model,
                                               PairGenerationOptions options)
    : m_gallery(gallery), m_model(model), m_options(options) {}

Result<std::vector<PairCandidate>> PairCandidateGenerator::Generate(const Tracklet& query) {
    m_stats = PairGenerationStats{};
    return GenerateOne(query, m_stats);
}

Result<std::vector<PairCandidate>> PairCandidateGenerator::Generate(
    std::span<const Tracklet> queries) {
    m_stats = PairGenerationStats{};
    std::vector<PairCandidate> all;
    for (const Tracklet& query : queries) {
        auto candidates = GenerateOne(query, m_stats);
        if (!candidates) {
            return candidates.status();
        }
        all.insert(all.end(), std::make_move_iterator(candidates.value().begin()),
                   std::make_move_iterator(candidates.value().end()));
    }
    return all;
}

Result<std::vector<PairCandidate>> PairCandidateGenerator::GenerateOne(
    const Tracklet& query, PairGenerationStats& stats) const {
    reid::MatcherOptions matcherOptions;
    matcherOptions.top_k =
        std::max<std::size_t>(1, m_options.top_k * std::max<std::size_t>(1, m_options.oversample));
    matcherOptions.min_similarity = m_options.min_similarity;
    matcherOptions.exclude_same_camera = m_options.exclude_same_camera;

    reid::ReIdMatcher matcher(m_gallery, matcherOptions);
    auto matches = matcher.Match(query);
    if (!matches) {
        return matches.status();
    }

    SpatialGateOptions gateOptions;
    gateOptions.mode = m_options.gate_mode;
    gateOptions.window = m_options.window;
    gateOptions.max_speed_mps = m_options.max_speed_mps;
    gateOptions.max_distance_m = m_options.max_distance_m;
    gateOptions.allow_missing_world = m_options.allow_missing_world;
    gateOptions.reject_same_camera = m_options.exclude_same_camera;
    const SpatialGate gate(m_model, gateOptions);

    ScorerOptions scorerOptions;
    scorerOptions.weights = m_options.weights;
    scorerOptions.window = m_options.window;
    scorerOptions.max_speed_mps = m_options.max_speed_mps;
    const StReIdScorer scorer(scorerOptions);

    const TrackletEndpoint queryEndpoint = MakeEndpoint(query);

    std::vector<PairCandidate> candidates;
    candidates.reserve(matches.value().size());
    for (const reid::MatchCandidate& match : matches.value()) {
        ++stats.considered;
        ++stats.appearance_candidates;

        const reid::GalleryEntry* entry = m_gallery.Get(match.candidate_id);
        if (entry == nullptr) {
            continue;
        }
        const TrackletEndpoint candidateEndpoint = MakeEndpoint(*entry);

        const GateDecision decision = gate.Check(queryEndpoint, candidateEndpoint);
        if (!decision.allowed) {
            CountGate(stats, decision.reason);
            continue;
        }

        std::optional<TransitionStats> transition;
        if (m_model != nullptr) {
            transition = m_model->Transition(query.camera, entry->camera);
        }
        const StCosts costs =
            scorer.Score(match.similarity, queryEndpoint, candidateEndpoint, transition);
        if (static_cast<double>(costs.total) > m_options.max_total_cost) {
            ++stats.rejected_cost;
            continue;
        }

        PairCandidate candidate;
        candidate.query_id = query.id;
        candidate.candidate_id = entry->id;
        candidate.from_camera = query.camera;
        candidate.to_camera = entry->camera;
        candidate.similarity = match.similarity;
        candidate.costs = costs;
        candidate.gate = GateReason::Allowed;
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), Better);
    if (candidates.size() > m_options.top_k) {
        stats.truncated += candidates.size() - m_options.top_k;
        candidates.resize(m_options.top_k);
    }
    stats.accepted += candidates.size();
    return candidates;
}

} // namespace tmcmt::st
