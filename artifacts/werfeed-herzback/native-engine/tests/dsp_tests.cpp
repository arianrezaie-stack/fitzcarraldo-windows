#include "Dsp.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <vector>

int main() {
    constexpr double rate = 48000.0;
    const auto sweep = werfeed::makeLogSweep(rate, 1.0);
    assert(sweep.size() == 48000);
    assert(*std::max_element(sweep.begin(), sweep.end()) <= 0.081f);

    std::vector<float> impulse(256, 0.0f), delayed(512, 0.0f);
    impulse[0] = 0.08f;
    delayed[137] = 0.08f;
    assert(werfeed::estimateDelay(impulse, delayed, 240) == 137);
    for (std::size_t i = 138; i < 190; ++i)
        delayed[i] = 0.025f * std::exp(-static_cast<float>(i - 138) / 18.0f);
    for (std::size_t i = 0; i < delayed.size(); ++i)
        delayed[i] += 0.001f * std::sin(static_cast<float>(i) * 1.731f);
    assert(werfeed::estimateDelay(std::span<const float>(impulse.data(), 64), delayed, 240, 0.2f) == 137);
    std::fill(delayed.begin(), delayed.end(), 0.0f);
    assert(werfeed::estimateDelay(impulse, delayed, 240) == -1);
    const auto probe = werfeed::makeDelayProbe();
    std::vector<float> noiseOnly(26000);
    unsigned noiseState = 17;
    for (auto& value : noiseOnly) {
        noiseState = noiseState * 1664525u + 1013904223u;
        value = (static_cast<float>((noiseState >> 8u) & 0xffffu) / 32768.0f - 1.0f) * 0.01f;
    }
    assert(werfeed::estimateDelay(probe, noiseOnly, 24000, 0.2f) == -1);

    std::vector<float> response(sweep.size() + 128, 0.0f);
    for (std::size_t i = 0; i < sweep.size(); ++i) response[i + 128] = sweep[i] * 0.5f;
    const auto measured = werfeed::measureResponse(sweep, response, rate, 128);
    for (const auto db : measured) assert(std::isfinite(db) && std::abs(db + 6.0206f) < 1.5f);

    werfeed::FeedbackProcessor processor;
    processor.prepare(rate);
    processor.setEnabled(true);
    std::array<float, werfeed::analyzerBins> baseline {};
    baseline.fill(-80.0f);
    processor.setBaseline(baseline);
    float previous = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        const auto input = 0.3f * std::sin(2.0f * werfeed::pi * 1000.0f * i / 48000.0f);
        const auto output = processor.process(input);
        assert(std::isfinite(output));
        assert(std::abs(output - previous) < 0.5f); // coefficient ramp remains click-free.
        previous = output;
    }
    const auto snapshot = processor.snapshot();
    assert(snapshot.activeNotches > 0);
    assert(snapshot.activeNotches <= static_cast<int>(werfeed::maxNotches));
    assert(snapshot.maximumCutDb >= -12.1f);
    const auto active = *std::min_element(snapshot.notches.begin(), snapshot.notches.end(),
        [](const werfeed::NotchSnapshot& a, const werfeed::NotchSnapshot& b) {
            const auto aDistance = a.active ? std::abs(std::log2(a.frequency / 1000.0f)) : 1000.0f;
            const auto bDistance = b.active ? std::abs(std::log2(b.frequency / 1000.0f)) : 1000.0f;
            return aDistance < bDistance;
        });
    double inputPower = 0.0, outputPower = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const auto input = 0.3f * std::sin(2.0f * werfeed::pi * active.frequency * i / 48000.0f);
        const auto output = processor.process(input);
        if (i > 24000) { inputPower += input * input; outputPower += output * output; }
    }
    assert(10.0 * std::log10(outputPower / inputPower) < -2.0);
    for (const auto frequency : { 125.0f, 997.0f, 8000.0f, 15731.0f }) {
        werfeed::FeedbackProcessor offGrid;
        offGrid.prepare(rate); offGrid.clearBaseline(); offGrid.setEnabled(true);
        for (int i = 0; i < 144000; ++i)
            offGrid.process(0.3f * std::sin(2.0f * werfeed::pi * frequency * i / 48000.0f));
        const auto offGridSnapshot = offGrid.snapshot();
        assert(offGridSnapshot.activeNotches > 0);
        assert(offGridSnapshot.maximumCutDb < -6.0f);
        assert(std::any_of(offGridSnapshot.notches.begin(), offGridSnapshot.notches.end(),
            [frequency](const werfeed::NotchSnapshot& notch) {
                return notch.active && std::abs(notch.frequency - frequency) / frequency < 0.2f;
            }));
    }
    werfeed::FeedbackProcessor twoTone;
    twoTone.prepare(rate); twoTone.clearBaseline(); twoTone.setEnabled(true);
    for (int i = 0; i < 144000; ++i) {
        const auto input = 0.2f * std::sin(2.0f * werfeed::pi * 984.375f * i / 48000.0f) +
                           0.2f * std::sin(2.0f * werfeed::pi * 3000.0f * i / 48000.0f);
        twoTone.process(input);
    }
    const auto twoToneSnapshot = twoTone.snapshot();
    assert(std::count_if(twoToneSnapshot.notches.begin(), twoToneSnapshot.notches.end(),
        [](const werfeed::NotchSnapshot& notch) { return notch.active; }) >= 2);

    // Bypass transitions remain continuous while wet filter history advances.
    float last = 0.0f;
    for (int i = 0; i < 12000; ++i) {
        if (i == 3000) processor.setEnabled(false);
        if (i == 7000) processor.setEnabled(true);
        const auto input = 0.25f * std::sin(2.0f * werfeed::pi * 1000.0f * i / 48000.0f);
        const auto output = processor.process(input);
        assert(std::isfinite(output));
        if (i > 0) assert(std::abs(output - last) < 0.2f);
        last = output;
    }

    // Short, moving tonal program peaks do not satisfy persistence.
    werfeed::FeedbackProcessor program;
    program.prepare(rate); program.clearBaseline(); program.setPreset(werfeed::ProtectionPreset::music);
    program.setEnabled(true);
    constexpr std::array<float, 8> notes { 220.0f, 329.63f, 440.0f, 587.33f,
                                           261.63f, 392.0f, 523.25f, 783.99f };
    for (int i = 0; i < 96000; ++i) {
        const auto note = notes[static_cast<std::size_t>(i / 3840) % notes.size()];
        const auto envelope = std::sin(werfeed::pi * static_cast<float>(i % 3840) / 3840.0f);
        const auto input = 0.22f * envelope * std::sin(2.0f * werfeed::pi * note * i / 48000.0f);
        program.process(input);
    }
    assert(program.snapshot().activeNotches == 0);
}