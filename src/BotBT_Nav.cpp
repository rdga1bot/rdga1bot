// BotBT_Nav.cpp
// Navigation helpers: deliverGeoPath, takePendingPathRequest,
// findBacktrackCrumb, addCrumb.
//
// addCrumb           — записує breadcrumb з дедуплікацією (min_dist guard).
// findBacktrackCrumb — шукає crumb за порогом відстані для backtrack.
// takePendingPathRequest — забирає pending A* запит (однократно).
// deliverGeoPath     — передає результат A* в BT стан.
//
// Crumb буфер: m_crumbs[] фіксований розмір, циклічне перезаписування.
#include "BotBehaviorTree.h"

void BotBehaviorTree::deliverGeoPath(
        const std::vector<std::pair<float,float>>& path, uint64_t id) {
    if (id <= m_tgt_geo_path_id) return;
    m_tgt_geo_path       = path;
    m_tgt_geo_path_idx   = 0;
    m_tgt_geo_path_id    = id;
    m_tgt_geo_path_ready = !path.empty();
}

std::optional<PathRequest> BotBehaviorTree::takePendingPathRequest() {
    if (!m_tgt_pending_path.has_value()) return std::nullopt;
    auto req = std::move(m_tgt_pending_path);
    m_tgt_pending_path = std::nullopt;
    return req;
}

void BotBehaviorTree::addCrumb(float x, float y, float z, const Config::BreadcrumbConfig& cfg) {
    if (!cfg.enabled) return;
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (!m_breadcrumbs.empty()) {
        float dx = x - m_breadcrumbs.back().x;
        float dy = y - m_breadcrumbs.back().y;
        if (dx*dx + dy*dy < cfg.record_distance * cfg.record_distance) return;
    }
    if ((int)m_breadcrumbs.size() >= cfg.max_count)
        m_breadcrumbs.pop_front();
    m_breadcrumbs.push_back({x, y, z});
}

std::optional<BotBehaviorTree::Crumb> BotBehaviorTree::findBacktrackCrumb(
        float px, float py, float range) const {
    for (int i = (int)m_breadcrumbs.size() - 2; i >= 0; --i) {
        const auto& c = m_breadcrumbs[i];
        float dx = c.x - px, dy = c.y - py;
        float d2 = dx*dx + dy*dy;
        if (d2 > 50.f*50.f && d2 <= range*range) return c;
    }
    return std::nullopt;
}
