#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace infalsus {

enum class FieldEasing : std::uint32_t {
    Linear = 4,
    SineOut = 8,
    SineIn = 16,
};

[[nodiscard]] constexpr std::uint32_t fieldEasingFlag(const FieldEasing easing, const bool rightEdge) {
    return static_cast<std::uint32_t>(easing) << (rightEdge ? 3 : 0);
}

[[nodiscard]] inline double fieldEase(const FieldEasing easing, const double progress) {
    const double value = std::clamp(progress, 0.0, 1.0);
    if (easing == FieldEasing::SineOut) {
        return std::sin(value * std::numbers::pi * 0.5);
    }
    if (easing == FieldEasing::SineIn) {
        return 1.0 - std::cos(value * std::numbers::pi * 0.5);
    }

    return value;
}

[[nodiscard]] inline double fieldEdgeProgress(const std::uint32_t auxiliary, const bool rightEdge,
    const double progress) {
    constexpr std::uint32_t kEasingMask = 28;
    const auto mode = static_cast<FieldEasing>((auxiliary >> (rightEdge ? 3 : 0)) & kEasingMask);

    return fieldEase(mode, progress);
}

} // namespace infalsus
