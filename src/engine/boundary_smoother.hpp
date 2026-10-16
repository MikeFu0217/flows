#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace flows {

// Playback-only correction at known discontinuities. Signal nodes and probes stay
// exact. No lookahead, filtering of steady audio, or allocation is required.
class BoundarySmoother {
    std::array<double, 2> last_{}, correction_{};
    std::uint32_t remaining_ = 0;
    bool pending_ = false;
public:
    void mark() noexcept { pending_ = true; }
    // Render the exact zero endpoint too, even when the preceding callback
    // ended on the last nonzero tail sample. Otherwise slot reuse sees stale audio.
    bool active() const noexcept { return pending_ || remaining_ != 0 || last_[0] != 0 || last_[1] != 0; }
    std::array<float, 2> tick(float left, float right, std::uint32_t length) noexcept {
        if (!std::isfinite(left) || !std::isfinite(right)) {
            *this = {};
            return {left, right}; // Let the engine's finite-output guard report it.
        }
        if (pending_) {
            correction_ = {last_[0] - left, last_[1] - right};
            remaining_ = length;
            pending_ = false;
        }
        double weight = 0;
        if (remaining_) {
            const double t = double(remaining_) / length;
            weight = t * t * (3 - 2 * t);
            --remaining_;
        }
        last_ = {left + correction_[0] * weight, right + correction_[1] * weight};
        return {static_cast<float>(last_[0]), static_cast<float>(last_[1])};
    }
};

} // namespace flows
