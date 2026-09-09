#include "Routing.h"
#include <array>
#include <cassert>

int main() {
    std::array<float, 4> in0 { 0.1f, -0.2f, 0.3f, -0.4f };
    std::array<float, 4> in1 { 0.5f, 0.5f, 0.5f, 0.5f };
    std::array<float, 4> out0 { 9, 9, 9, 9 }, out1 { 9, 9, 9, 9 };
    const float* inputs[] = { in0.data(), in1.data() };
    float* outputs[] = { out0.data(), out1.data() };
    std::array<werfeed::Route, werfeed::maxRoutes> routes {};
    routes[0] = { 0, 1 };
    routes[1] = { 1, 0 };
    werfeed::routeMono(inputs, 2, outputs, 2, 4, routes, 2);
    assert(out0 == in1);
    assert(out1 == in0);
    routes[0] = { 9, 0 }; // invalid routes must be ignored safely.
    werfeed::routeMono(inputs, 2, outputs, 2, 4, routes, 1);
    assert((out0 == std::array<float, 4> { 0, 0, 0, 0 }));

    // Calibration exclusively owns its selected output and silences every
    // ordinary/shared route so program audio cannot exceed the safe stimulus.
    std::array<float, 4> excitation { 0.02f, -0.03f, 0.04f, -0.05f };
    std::array<float, 8> recording {};
    out0.fill(9.0f); out1.fill(9.0f);
    werfeed::routeCalibration(inputs, 2, outputs, 2, 4, { 0, 1 },
        excitation, recording, 0);
    assert((out0 == std::array<float, 4> { 0, 0, 0, 0 }));
    assert(out1 == excitation);
    assert(std::equal(in0.begin(), in0.end(), recording.begin()));
}