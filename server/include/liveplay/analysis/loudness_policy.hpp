#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

namespace liveplay::analysis {
struct LoudnessSafetySettings {
    double target_lufs = -12.0;
    double max_positive_gain_db = 6.0;
    double max_negative_gain_db = -12.0;
    double true_peak_ceiling_dbtp = -1.0;
};
struct LoudnessGainPreview {
    bool valid = false;
    double gain_db = 0.0;
    std::optional<double> estimated_true_peak_dbtp;
};
inline LoudnessGainPreview safe_loudness_gain(double integrated_lufs,
    std::optional<double> true_peak_dbtp, const LoudnessSafetySettings& settings = {}) {
    if (!std::isfinite(integrated_lufs)) return {};
    double gain = std::clamp(settings.target_lufs - integrated_lufs,
        settings.max_negative_gain_db, settings.max_positive_gain_db);
    if (true_peak_dbtp && std::isfinite(*true_peak_dbtp)) {
        gain = std::min(gain, settings.true_peak_ceiling_dbtp - *true_peak_dbtp);
        if (gain < settings.max_negative_gain_db) return {};
        return {true, gain, *true_peak_dbtp + gain};
    }
    return {true, gain, std::nullopt};
}
} // namespace liveplay::analysis
