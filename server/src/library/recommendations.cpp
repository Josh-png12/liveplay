#include "liveplay/library/recommendations.hpp"
#include <algorithm>
#include <cmath>

namespace liveplay::library {
std::vector<Track> RuleBasedRecommendationProvider::recommend(const std::vector<Track>& tracks,
                                                               const RecommendationRequest& request) const {
    struct Ranked { Track track; int score; };
    std::vector<Ranked> ranked;
    for (const auto& track : tracks) {
        if (track.status != "available") continue;
        if (request.require_school_approved && !track.tags.school_approved) continue;
        int score = track.tags.school_approved ? 8 : 0;
        if (!request.event_role.empty()) {
            const bool role = std::find(track.tags.event_roles.begin(), track.tags.event_roles.end(), request.event_role) != track.tags.event_roles.end();
            score += role ? 30 : -8;
        }
        if (request.target_energy >= 1 && request.target_energy <= 5) {
            score += track.tags.energy ? std::max(0, 15 - 4 * std::abs(track.tags.energy - request.target_energy)) : 0;
        }
        if (!request.avoid_artist.empty() && !track.artist.empty() && track.artist == request.avoid_artist) score -= 20;
        ranked.push_back({track, score});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.score != b.score ? a.score > b.score : a.track.id < b.track.id;
    });
    std::vector<Track> result; result.reserve(std::min(request.limit, ranked.size()));
    for (const auto& item : ranked) { if (result.size() == request.limit) break; result.push_back(item.track); }
    return result;
}
} // namespace liveplay::library
