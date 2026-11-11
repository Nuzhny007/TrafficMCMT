#include "trafficmcmt/pipeline/ReplayGuard.h"

#include <utility>

namespace tmcmt::pipeline {

ReplayGuard::ReplayGuard(ReplayGuardOptions options) : m_options(std::move(options)) {
    if (m_options.max_seen_keys == 0) {
        m_options.max_seen_keys = 1;
    }
}

ArrivalClass ReplayGuard::Classify(TrackletId id, Timestamp event_time, Timestamp watermark,
                                   bool has_watermark, WindowState window_state) {
    if (!Remember(id)) {
        ++m_stats.duplicates;
        return ArrivalClass::Duplicate;
    }

    if (!has_watermark || event_time >= watermark) {
        ++m_stats.in_order;
        return ArrivalClass::InOrder;
    }

    switch (window_state) {
    case WindowState::Pending:
        // The window has not been solved yet; the item will be included when
        // it closes, so this is a normal out-of-order arrival.
        ++m_stats.in_order;
        return ArrivalClass::InOrder;
    case WindowState::Emitted:
        ++m_stats.late;
        return ArrivalClass::LateWithinWindow;
    case WindowState::Committed:
        ++m_stats.too_late;
        return ArrivalClass::TooLate;
    }
    ++m_stats.too_late;
    return ArrivalClass::TooLate;
}

bool ReplayGuard::Remember(TrackletId id) {
    if (!m_seen.insert(id).second) {
        return false;
    }
    m_order.push_back(id);
    while (m_order.size() > m_options.max_seen_keys) {
        m_seen.erase(m_order.front());
        m_order.pop_front();
    }
    return true;
}

void ReplayGuard::Reset() {
    m_seen.clear();
    m_order.clear();
    m_stats = ReplayGuardStats{};
}

} // namespace tmcmt::pipeline
