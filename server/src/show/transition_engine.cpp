#include "liveplay/show/transition_engine.hpp"
#include "liveplay/core/project_state.hpp"

namespace liveplay::show {
bool BasicTransitionEngine::transition(core::ProjectState& state, const std::string& from_item,
                                       const std::string& to_item, const TransitionSpec& spec) {
    if (to_item.empty()) return false;
    if (spec.type == TransitionType::Cut) {
        if (!from_item.empty()) state.stop_item(from_item, 0);
        return state.play_item(to_item);
    }
    if (spec.type == TransitionType::Fade) {
        if (!from_item.empty()) state.stop_item(from_item, spec.duration.count());
        return state.play_item(to_item);
    }
    // ProjectState already supports overlapping cue fades and routes; starting
    // the incoming cue before fading the outgoing cue is the V0 crossfade.
    const bool started = state.play_item(to_item);
    if (!from_item.empty()) state.stop_item(from_item, spec.duration.count());
    return started;
}
} // namespace liveplay::show
