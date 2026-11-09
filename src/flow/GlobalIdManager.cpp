#include "trafficmcmt/flow/GlobalIdManager.h"

#include <algorithm>
#include <utility>

namespace tmcmt::flow {

GlobalIdManager::GlobalIdManager(GlobalIdManagerOptions options)
    : m_options(std::move(options)),
      m_next_id(m_options.first_id == kInvalidGlobalId ? 1 : m_options.first_id) {
    if (m_options.commit_lag < Timestamp{}) {
        m_options.commit_lag = Timestamp{};
    }
}

std::size_t GlobalIdManager::Find(std::size_t index) {
    std::size_t root = index;
    while (m_elements[root].parent != root) {
        root = m_elements[root].parent;
    }
    while (m_elements[index].parent != index) {
        const std::size_t next = m_elements[index].parent;
        m_elements[index].parent = root;
        index = next;
    }
    return root;
}

std::size_t GlobalIdManager::Find(std::size_t index) const {
    while (m_elements[index].parent != index) {
        index = m_elements[index].parent;
    }
    return index;
}

Result<GlobalId> GlobalIdManager::Add(TrackletId id, Timestamp end) {
    if (id == 0) {
        return Status::Error(ErrorCode::InvalidArgument, "tracklet id must be non-zero");
    }
    const auto it = m_index.find(id);
    if (it != m_index.end()) {
        Element& element = m_elements[it->second];
        if (end > element.max_end) {
            element.max_end = end;
        }
        return IdFor(id);
    }

    Element element;
    element.parent = m_elements.size();
    element.rank = 0;
    element.global_id = m_next_id;
    element.max_end = end;
    element.committed = false;
    ++m_next_id;
    m_index.emplace(id, m_elements.size());
    m_elements.push_back(element);
    return element.global_id;
}

Status GlobalIdManager::Link(TrackletId a, TrackletId b, Timestamp ts, std::string reason) {
    const auto first = m_index.find(a);
    const auto second = m_index.find(b);
    if (first == m_index.end()) {
        return Status::Error(ErrorCode::NotFound,
                             "unknown tracklet " + std::to_string(a));
    }
    if (second == m_index.end()) {
        return Status::Error(ErrorCode::NotFound,
                             "unknown tracklet " + std::to_string(b));
    }

    const std::size_t rootA = Find(first->second);
    const std::size_t rootB = Find(second->second);
    if (rootA == rootB) {
        if (ts > m_elements[rootA].max_end) {
            m_elements[rootA].max_end = ts;
        }
        return Status::Ok();
    }

    const bool committedA = m_elements[rootA].committed;
    const bool committedB = m_elements[rootB].committed;
    const GlobalId idA = m_elements[rootA].global_id;
    const GlobalId idB = m_elements[rootB].global_id;

    GlobalId keep = kInvalidGlobalId;
    GlobalId retire = kInvalidGlobalId;
    if (committedA && !committedB) {
        keep = idA;
        retire = idB;
    } else if (committedB && !committedA) {
        keep = idB;
        retire = idA;
    } else {
        keep = std::min(idA, idB);
        retire = std::max(idA, idB);
    }

    std::size_t newRoot = rootA;
    std::size_t oldRoot = rootB;
    if (m_elements[rootA].rank < m_elements[rootB].rank) {
        newRoot = rootB;
        oldRoot = rootA;
    } else if (m_elements[rootA].rank == m_elements[rootB].rank) {
        ++m_elements[rootA].rank;
    }

    const Timestamp mergedEnd = std::max(m_elements[rootA].max_end, m_elements[rootB].max_end);
    m_elements[oldRoot].parent = newRoot;
    m_elements[newRoot].global_id = keep;
    m_elements[newRoot].max_end = mergedEnd;
    m_elements[newRoot].committed = committedA || committedB;

    if (keep != retire) {
        m_events.push_back(
            GlobalIdMergeEvent{retire, keep, reason.empty() ? "link" : std::move(reason), ts});
    }
    if (committedA || committedB) {
        m_committed_ids.insert(keep);
        const bool retireWasCommitted = (retire == idA && committedA) ||
                                        (retire == idB && committedB);
        if (retireWasCommitted) {
            m_committed_ids.insert(retire);
        }
    }
    return Status::Ok();
}

Status GlobalIdManager::Commit(Timestamp now) {
    const Timestamp cutoff = now - m_options.commit_lag;
    for (std::size_t index = 0; index < m_elements.size(); ++index) {
        Element& element = m_elements[index];
        if (element.parent != index || element.committed) {
            continue;
        }
        if (element.max_end <= cutoff) {
            element.committed = true;
            m_committed_ids.insert(element.global_id);
        }
    }
    return Status::Ok();
}

GlobalId GlobalIdManager::IdFor(TrackletId id) const noexcept {
    const auto it = m_index.find(id);
    if (it == m_index.end()) {
        return kInvalidGlobalId;
    }
    return m_elements[Find(it->second)].global_id;
}

bool GlobalIdManager::Contains(TrackletId id) const noexcept {
    return m_index.find(id) != m_index.end();
}

bool GlobalIdManager::IsCommitted(GlobalId id) const noexcept {
    return m_committed_ids.find(id) != m_committed_ids.end();
}

std::size_t GlobalIdManager::GroupCount() const {
    std::size_t groups = 0;
    for (std::size_t index = 0; index < m_elements.size(); ++index) {
        if (m_elements[index].parent == index) {
            ++groups;
        }
    }
    return groups;
}

std::vector<std::pair<TrackletId, GlobalId>> GlobalIdManager::Assignments() const {
    std::vector<std::pair<TrackletId, GlobalId>> assignments;
    assignments.reserve(m_index.size());
    for (const auto& [tracklet, index] : m_index) {
        assignments.emplace_back(tracklet, m_elements[Find(index)].global_id);
    }
    std::sort(assignments.begin(), assignments.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.first < rhs.first;
              });
    return assignments;
}

void GlobalIdManager::Clear() {
    m_elements.clear();
    m_index.clear();
    m_committed_ids.clear();
    m_events.clear();
    m_next_id = m_options.first_id == kInvalidGlobalId ? 1 : m_options.first_id;
}

} // namespace tmcmt::flow
