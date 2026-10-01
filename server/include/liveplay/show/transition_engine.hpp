#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace liveplay::core { class ProjectState; }
namespace liveplay::show {

enum class TransitionType { Cut, Fade, Crossfade };
struct TransitionSpec { TransitionType type = TransitionType::Cut; std::chrono::milliseconds duration{0}; };

// A single control-plane seam for future beatmatched/harmonic transitions.
// Implementations schedule existing ProjectState/AudioEngine operations; they
// never own a playback graph or execute on a render callback.
class TransitionEngine {
public:
    virtual ~TransitionEngine() = default;
    virtual bool transition(core::ProjectState& state, const std::string& from_item,
                            const std::string& to_item, const TransitionSpec& spec) = 0;
};
class BasicTransitionEngine final : public TransitionEngine {
public:
    bool transition(core::ProjectState& state, const std::string& from_item,
                    const std::string& to_item, const TransitionSpec& spec) override;
};
} // namespace liveplay::show
