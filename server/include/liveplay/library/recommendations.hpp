#pragma once

#include "liveplay/library/track.hpp"
#include <memory>
#include <string>
#include <vector>

namespace liveplay::library {

struct RecommendationRequest {
    std::string event_role;
    int target_energy = 0;
    std::string avoid_artist;
    bool require_school_approved = false;
    std::size_t limit = 20;
};

class TrackRecommendationProvider {
public:
    virtual ~TrackRecommendationProvider() = default;
    virtual std::string id() const = 0;
    virtual std::vector<Track> recommend(const std::vector<Track>& tracks,
                                         const RecommendationRequest& request) const = 0;
};

// Deterministic baseline: operator tags are authoritative and the stable Track
// ID makes equal scores reproducible. Future MIR/CLAP providers can compose
// with it without changing its HTTP or UI contract.
class RuleBasedRecommendationProvider final : public TrackRecommendationProvider {
public:
    std::string id() const override { return "rule-based"; }
    std::vector<Track> recommend(const std::vector<Track>& tracks,
                                 const RecommendationRequest& request) const override;
};

} // namespace liveplay::library
