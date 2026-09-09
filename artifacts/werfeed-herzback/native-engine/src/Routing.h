#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <span>

namespace werfeed {

constexpr std::size_t maxRoutes = 8;

struct Route {
    int input = -1;
    int output = -1;
};

// This function is intentionally JUCE-free and allocation-free. It is used by
// the audio callback and by the portable test target.
inline void routeMono(const float* const* inputs, int inputChannels,
                      float* const* outputs, int outputChannels,
                      int samples, const std::array<Route, maxRoutes>& routes,
                      int routeCount) noexcept {
    for (int output = 0; output < outputChannels; ++output)
        std::fill_n(outputs[output], samples, 0.0f);

    for (int i = 0; i < routeCount; ++i) {
        const auto route = routes[static_cast<std::size_t>(i)];
        if (route.input < 0 || route.input >= inputChannels ||
            route.output < 0 || route.output >= outputChannels)
            continue;
        const float* source = inputs[route.input];
        float* destination = outputs[route.output];
        for (int frame = 0; frame < samples; ++frame)
            destination[frame] += source[frame];
    }
}

inline void routeCalibration(const float* const* inputs, int inputChannels,
                             float* const* outputs, int outputChannels, int samples,
                             Route route, std::span<const float> excitation,
                             std::span<float> recording, std::size_t position) noexcept {
    for (int output = 0; output < outputChannels; ++output)
        std::fill_n(outputs[output], samples, 0.0f);
    if (route.input < 0 || route.input >= inputChannels ||
        route.output < 0 || route.output >= outputChannels)
        return;
    for (int frame = 0; frame < samples; ++frame) {
        const auto index = position + static_cast<std::size_t>(frame);
        if (index < recording.size()) recording[index] = inputs[route.input][frame];
        outputs[route.output][frame] = index < excitation.size() ? excitation[index] : 0.0f;
    }
}

} // namespace werfeed