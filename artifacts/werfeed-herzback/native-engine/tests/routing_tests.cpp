#include "Routing.h"
#include <array>
#include <iostream>

#define REQUIRE(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            std::cerr << "routing-tests: check failed: " << #__VA_ARGS__ \
                      << " (line " << __LINE__ << ")\n"; \
            return 1; \
        } \
    } while (false)

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
    REQUIRE(out0 == in1);
    REQUIRE(out1 == in0);
    routes[0] = { 9, 0 }; // invalid routes must be ignored safely.
    werfeed::routeMono(inputs, 2, outputs, 2, 4, routes, 1);
    REQUIRE((out0 == std::array<float, 4> { 0, 0, 0, 0 }));

    // Calibration exclusively owns its selected output and silences every
    // ordinary/shared route so program audio cannot exceed the safe stimulus.
    std::array<float, 4> excitation { 0.02f, -0.03f, 0.04f, -0.05f };
    std::array<float, 8> recording {};
    out0.fill(9.0f); out1.fill(9.0f);
    werfeed::routeCalibration(inputs, 2, outputs, 2, 4, { 0, 1 },
        excitation, recording, 0);
    REQUIRE((out0 == std::array<float, 4> { 0, 0, 0, 0 }));
    REQUIRE(out1 == excitation);
    REQUIRE(std::equal(in0.begin(), in0.end(), recording.begin()));

    // The optional announcement plays on the selected output, then a silent
    // one-second gap precedes the measured calibration excitation.
    std::array<float, 2> announcement { 0.7f, 0.8f };
    std::array<float, 2> announcedExcitation { 0.2f, 0.3f };
    std::array<float, 4> announcedRecording {};
    out0.fill(9.0f); out1.fill(9.0f);
    werfeed::routeCalibrationWithAnnouncement(inputs, 2, outputs, 2, 2, { 0, 1 },
        announcedExcitation, announcedRecording, announcement, 2, 0);
    REQUIRE((std::array<float, 2> { out0[0], out0[1] } == std::array<float, 2> { 0, 0 }));
    REQUIRE((std::array<float, 2> { out1[0], out1[1] } == announcement));
    out0.fill(9.0f); out1.fill(9.0f);
    werfeed::routeCalibrationWithAnnouncement(inputs, 2, outputs, 2, 2, { 0, 1 },
        announcedExcitation, announcedRecording, announcement, 2, 2);
    REQUIRE((std::array<float, 2> { out1[0], out1[1] } == std::array<float, 2> { 0, 0 }));
    out0.fill(9.0f); out1.fill(9.0f);
    werfeed::routeCalibrationWithAnnouncement(inputs, 2, outputs, 2, 4, { 0, 1 },
        announcedExcitation, announcedRecording, announcement, 2, 4);
    REQUIRE((out1 == std::array<float, 4> { 0.2f, 0.3f, 0, 0 }));
    REQUIRE(std::equal(in0.begin(), in0.end(), announcedRecording.begin()));

    // A four-channel interface can carry four independent mono routes in the
    // same callback. Route order is preserved even when the mappings cross.
    std::array<float, 4> in2 { -0.6f, -0.7f, -0.8f, -0.9f };
    std::array<float, 4> in3 { 0.9f, 0.8f, 0.7f, 0.6f };
    std::array<float, 4> out2 { 0, 0, 0, 0 }, out3 { 0, 0, 0, 0 };
    const float* fourInputs[] = { in0.data(), in1.data(), in2.data(), in3.data() };
    float* fourOutputs[] = { out0.data(), out1.data(), out2.data(), out3.data() };
    routes[0] = { 0, 3 };
    routes[1] = { 1, 2 };
    routes[2] = { 2, 1 };
    routes[3] = { 3, 0 };
    werfeed::routeMono(fourInputs, 4, fourOutputs, 4, 4, routes, 4);
    REQUIRE(out0 == in3);
    REQUIRE(out1 == in2);
    REQUIRE(out2 == in1);
    REQUIRE(out3 == in0);
    return 0;
}