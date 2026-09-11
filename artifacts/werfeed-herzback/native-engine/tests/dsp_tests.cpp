#include "Dsp.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr double calibrationStartHz = 20.0;
constexpr double calibrationEndHz = 20000.0;
static_assert(sizeof(werfeed::FeedbackProcessor) < 200000,
              "FeedbackProcessor must remain small enough for Windows test stacks");

double shapedRoomResponseDb(double frequency) {
    const auto position = std::log(frequency / calibrationStartHz) /
        std::log(calibrationEndHz / calibrationStartHz);
    // A broad, deterministic room curve: rising high-frequency response with
    // two wide room modes. Its features are intentionally wider than the
    // calibration analysis window so local measurement should track them.
    return -2.0 + 5.0 * position +
        2.5 * std::sin(2.0 * werfeed::pi * 2.0 * position);
}

float processSample(werfeed::FeedbackProcessor& processor, float input) {
    processor.pullPendingNotchUpdates();
    const auto output = processor.process(input);
    processor.pushAnalysisBlock(&input, 1);
    while (processor.pumpBackgroundAnalysis()) {}
    return output;
}

} // namespace

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
    // Silence/noise below the correlation threshold must not produce a delay.
    assert(werfeed::estimateDelay(probe, noiseOnly, 24000, 0.2f) == -1);

    std::vector<float> response(sweep.size() + 128, 0.0f);
    for (std::size_t i = 0; i < sweep.size(); ++i) response[i + 128] = sweep[i] * 0.5f;
    const auto measured = werfeed::measureResponse(sweep, response, rate, 128);
    // Flat gain remains the baseline calibration behavior.
    for (const auto db : measured) assert(std::isfinite(db) && std::abs(db + 6.0206f) < 1.5f);

    constexpr std::size_t shapedDelay = 173;
    std::vector<float> shapedResponse(sweep.size() + shapedDelay, 0.0f);
    for (std::size_t i = 0; i < sweep.size(); ++i) {
        const auto position = static_cast<double>(i) /
            static_cast<double>(sweep.size() - 1);
        const auto frequency = calibrationStartHz *
            std::pow(calibrationEndHz / calibrationStartHz, position);
        const auto gain = std::pow(10.0, shapedRoomResponseDb(frequency) / 20.0);
        shapedResponse[i + shapedDelay] = sweep[i] * static_cast<float>(gain);
    }
    const auto shapedMeasured = werfeed::measureResponse(
        sweep, shapedResponse, rate, static_cast<int>(shapedDelay));
    // The 40 ms local analysis window averages a small portion of the sweep,
    // so the measured curve must follow the known response within 1.0 dB.
    for (std::size_t bin = 0; bin < werfeed::analyzerBins; ++bin) {
        const auto position = static_cast<double>(bin) /
            static_cast<double>(werfeed::analyzerBins - 1);
        const auto frequency = calibrationStartHz *
            std::pow(calibrationEndHz / calibrationStartHz, position);
        assert(std::isfinite(shapedMeasured[bin]));
        assert(std::abs(shapedMeasured[bin] -
                        static_cast<float>(shapedRoomResponseDb(frequency))) < 1.0f);
    }
    std::array<float, werfeed::analyzerBins> calibrationPeaks {};
    calibrationPeaks.fill(0.0f);
    calibrationPeaks[96] = 12.0f;
    const auto calibratedBaseline = werfeed::detectionBaseline(calibrationPeaks);
    const auto calibratedBias = werfeed::calibrationBiasFromResponse(calibrationPeaks);
    assert(calibratedBaseline[96] < calibratedBaseline[95]);
    assert(calibratedBaseline[96] <= -61.0f);
    assert(calibratedBaseline[20] == -55.0f);
    assert(calibratedBias[96] >= 6.0f);
    assert(calibratedBias[95] > 0.0f);
    assert(calibratedBias[94] > 0.0f);
    assert(calibratedBias[93] == 0.0f);
    assert(std::abs(werfeed::notchQuality(100.0f) - 9.0f) < 0.01f);
    assert(std::abs(werfeed::notchQuality(500.0f) - 11.0f) < 0.01f);
    assert(std::abs(werfeed::notchQuality(1000.0f) - 14.0f) < 0.01f);
    assert(std::abs(werfeed::notchQuality(4000.0f) - 20.0f) < 0.01f);
    assert(std::abs(werfeed::notchQuality(10000.0f) - 30.0f) < 0.01f);

    werfeed::FeedbackProcessor processor;
    processor.prepare(rate);
    processor.setEnabled(true);
    processor.setSuppressionAmount(1.0f);
    std::array<float, werfeed::analyzerBins> baseline {};
    baseline.fill(-80.0f);
    processor.setBaseline(baseline);
    float previous = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        const auto input = 0.3f * std::sin(2.0f * werfeed::pi * 1000.0f * i / 48000.0f);
        const auto output = processSample(processor, input);
        assert(std::isfinite(output));
        assert(std::abs(output - previous) < 0.5f); // coefficient ramp remains click-free.
        previous = output;
    }
    const auto snapshot = processor.snapshot();
    assert(snapshot.activeNotches > 0);
    assert(snapshot.activeNotches <= static_cast<int>(werfeed::maxNotches));
    assert(snapshot.maximumCutDb >= -24.1f && snapshot.maximumCutDb <= -23.4f);
    const auto active = *std::min_element(snapshot.notches.begin(), snapshot.notches.end(),
        [](const werfeed::NotchSnapshot& a, const werfeed::NotchSnapshot& b) {
            const auto aDistance = a.active ? std::abs(std::log2(a.frequency / 1000.0f)) : 1000.0f;
            const auto bDistance = b.active ? std::abs(std::log2(b.frequency / 1000.0f)) : 1000.0f;
            return aDistance < bDistance;
        });
    double inputPower = 0.0, outputPower = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const auto input = 0.3f * std::sin(2.0f * werfeed::pi * active.frequency * i / 48000.0f);
        const auto output = processSample(processor, input);
        if (i > 24000) { inputPower += input * input; outputPower += output * output; }
    }
    assert(10.0 * std::log10(outputPower / inputPower) < -2.0);
    for (const auto frequency : { 125.0f, 997.0f, 8000.0f, 15731.0f }) {
        werfeed::FeedbackProcessor offGrid;
        offGrid.prepare(rate); offGrid.clearBaseline(); offGrid.setEnabled(true);
        for (int i = 0; i < 144000; ++i)
            processSample(offGrid, 0.3f * std::sin(2.0f * werfeed::pi * frequency * i / 48000.0f));
        const auto offGridSnapshot = offGrid.snapshot();
        assert(offGridSnapshot.activeNotches > 0);
        assert(offGridSnapshot.maximumCutDb < -6.0f);
        assert(std::any_of(offGridSnapshot.notches.begin(), offGridSnapshot.notches.end(),
            [frequency](const werfeed::NotchSnapshot& notch) {
                 return notch.active && std::abs(notch.frequency - frequency) /
                     frequency < 0.02f;
            }));
    }
    // A lower-level feedback tone should engage below the old 12 dB speech
    // gate while still passing the tonal and persistence checks.
    werfeed::FeedbackProcessor quietFeedback;
    quietFeedback.prepare(rate); quietFeedback.setEnabled(true);
    int firstEngagedSample = -1;
    for (int i = 0; i < 144000; ++i) {
        processSample(quietFeedback, 0.006f * std::sin(2.0f * werfeed::pi * 1000.0f * i / 48000.0f));
        if (firstEngagedSample < 0 && quietFeedback.snapshot().activeNotches > 0)
            firstEngagedSample = i;
    }
    assert(firstEngagedSample >= 0 && firstEngagedSample < 4500);
    assert(quietFeedback.snapshot().activeNotches > 0);
    // Notch release is generic behavior, not a special case for 70 Hz:
    // exercise it at two representative low-frequency tones.
    for (const auto frequency : { 70.0f, 137.0f }) {
        werfeed::FeedbackProcessor releaseTone;
        releaseTone.prepare(rate); releaseTone.clearBaseline(); releaseTone.setEnabled(true);
        releaseTone.setSuppressionAmount(1.0f);
        for (int i = 0; i < 144000; ++i)
            processSample(releaseTone, 0.3f * std::sin(2.0f * werfeed::pi * frequency * i / 48000.0f));
        const auto releaseSnapshot = releaseTone.snapshot();
        assert(releaseSnapshot.activeNotches == 1);
        assert(releaseSnapshot.maximumCutDb <= -23.4f);
        assert(std::any_of(releaseSnapshot.notches.begin(), releaseSnapshot.notches.end(),
            [frequency](const werfeed::NotchSnapshot& notch) {
                return notch.active && std::abs(notch.frequency - frequency) / frequency < 0.02f
                    && notch.q < 10.0f;
            }));
        for (int i = 0; i < 96000; ++i) processSample(releaseTone, 0.0f);
        assert(releaseTone.snapshot().activeNotches == 1);
        for (int i = 0; i < 300000; ++i) processSample(releaseTone, 0.0f);
        assert(releaseTone.snapshot().activeNotches == 0);
    }
    // The speech scale reaches -14 dB at 70% and -24 dB at full depth.
    werfeed::FeedbackProcessor seventyPercent;
    seventyPercent.prepare(rate); seventyPercent.setBaseline(baseline);
    seventyPercent.setEnabled(true); seventyPercent.setSuppressionAmount(0.7f);
    for (int i = 0; i < 96000; ++i)
        processSample(seventyPercent, 0.3f * std::sin(
            2.0f * werfeed::pi * 1000.0f * i / 48000.0f));
    const auto seventySnapshot = seventyPercent.snapshot();
    assert(seventySnapshot.maximumCutDb >= -14.1f &&
           seventySnapshot.maximumCutDb <= -13.4f);
    // The upper 30% of Speech lowers the threshold beyond the former maximum.
    werfeed::FeedbackProcessor thresholdAtSeventy;
    thresholdAtSeventy.prepare(rate); thresholdAtSeventy.clearBaseline();
    thresholdAtSeventy.setEnabled(true); thresholdAtSeventy.setSuppressionAmount(0.7f);
    werfeed::FeedbackProcessor thresholdAtHundred;
    thresholdAtHundred.prepare(rate); thresholdAtHundred.clearBaseline();
    thresholdAtHundred.setEnabled(true); thresholdAtHundred.setSuppressionAmount(1.0f);
    for (int i = 0; i < 96000; ++i) {
        const auto borderlineTone = 0.003f * std::sin(
            2.0f * werfeed::pi * 1000.0f * i / 48000.0f);
        processSample(thresholdAtSeventy, borderlineTone);
        processSample(thresholdAtHundred, borderlineTone);
    }
    assert(thresholdAtSeventy.snapshot().activeNotches == 0);
    assert(thresholdAtHundred.snapshot().activeNotches > 0);

    // Above 1 kHz, the same borderline tone should cross the lower
    // frequency-weighted gate sooner.
    werfeed::FeedbackProcessor lowFrequency;
    lowFrequency.prepare(rate); lowFrequency.clearBaseline();
    lowFrequency.setEnabled(true); lowFrequency.setSuppressionAmount(0.5f);
    werfeed::FeedbackProcessor highFrequency;
    highFrequency.prepare(rate); highFrequency.clearBaseline();
    highFrequency.setEnabled(true); highFrequency.setSuppressionAmount(0.5f);
    for (int i = 0; i < 96000; ++i) {
        processSample(lowFrequency, 0.0035f * std::sin(
            2.0f * werfeed::pi * 1000.0f * i / 48000.0f));
        processSample(highFrequency, 0.0035f * std::sin(
            2.0f * werfeed::pi * 4000.0f * i / 48000.0f));
    }
    assert(lowFrequency.snapshot().activeNotches == 0);
    assert(highFrequency.snapshot().activeNotches > 0);

    // A calibrated resonance remains protected longer than an ordinary tone.
    constexpr float hotspotFrequency = 1000.0f;
    const auto hotspotBin = static_cast<std::size_t>(std::lround(
        std::log10(hotspotFrequency / 20.0f) / std::log10(1000.0f) *
        static_cast<float>(werfeed::analyzerBins - 1)));
    std::array<float, werfeed::analyzerBins> hotspotResponse {};
    hotspotResponse.fill(0.0f);
    hotspotResponse[hotspotBin] = 14.0f;
    werfeed::FeedbackProcessor calibratedHotspot;
    calibratedHotspot.prepare(rate);
    calibratedHotspot.setCalibrationProfile(hotspotResponse);
    calibratedHotspot.setEnabled(true);
    calibratedHotspot.setSuppressionAmount(1.0f);
    for (int i = 0; i < 96000; ++i)
        processSample(calibratedHotspot, 0.04f * std::sin(
            2.0f * werfeed::pi * hotspotFrequency * i / 48000.0f));
    assert(calibratedHotspot.snapshot().activeNotches > 0);
    for (int i = 0; i < 24000; ++i) processSample(calibratedHotspot, 0.0f);
    assert(calibratedHotspot.snapshot().activeNotches > 0);
    for (int i = 0; i < 240000; ++i) processSample(calibratedHotspot, 0.0f);
    assert(calibratedHotspot.snapshot().activeNotches > 0);
    for (int i = 0; i < 240000; ++i) processSample(calibratedHotspot, 0.0f);
    assert(calibratedHotspot.snapshot().activeNotches == 0);
    werfeed::FeedbackProcessor twoTone;
    twoTone.prepare(rate); twoTone.clearBaseline(); twoTone.setEnabled(true);
    for (int i = 0; i < 144000; ++i) {
        const auto input = 0.2f * std::sin(2.0f * werfeed::pi * 984.375f * i / 48000.0f) +
                           0.2f * std::sin(2.0f * werfeed::pi * 3000.0f * i / 48000.0f);
        processSample(twoTone, input);
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
        const auto output = processSample(processor, input);
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
        processSample(program, input);
    }
    assert(program.snapshot().activeNotches == 0);

    // A malformed input sample must never propagate a non-finite output.
    assert(std::isfinite(processSample(program, std::numeric_limits<float>::quiet_NaN())));
    assert(std::isfinite(processSample(program, std::numeric_limits<float>::infinity())));
}