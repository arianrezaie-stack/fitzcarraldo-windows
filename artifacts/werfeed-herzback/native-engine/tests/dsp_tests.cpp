#include "Dsp.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace {

#define REQUIRE(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            std::cerr << "dsp-tests: check failed: " << #__VA_ARGS__ \
                      << " (line " << __LINE__ << ")\n"; \
            return 1; \
        } \
    } while (false)

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

void processConfirmedTone(werfeed::FeedbackProcessor& processor,
                          float frequency, float amplitude,
                          int samples = 144000) {
    const auto probeDrop = std::pow(10.0f,
        werfeed::maximumSuppressionDepth(processor.getSuppressionAmount()) *
        0.5f / 20.0f);
    for (int i = 0; i < samples; ++i) {
        const auto level = i < 4096 ? amplitude : amplitude * probeDrop;
        processSample(processor, level * std::sin(
            2.0f * werfeed::pi * frequency * i / 48000.0f));
    }
}

template <std::size_t Count>
void processConfirmedTones(werfeed::FeedbackProcessor& processor,
                           const std::array<float, Count>& frequencies,
                           float amplitude, int samples = 144000) {
    const auto probeDrop = std::pow(10.0f,
        werfeed::maximumSuppressionDepth(processor.getSuppressionAmount()) *
        0.5f / 20.0f);
    for (int i = 0; i < samples; ++i) {
        const auto level = i < 4096 ? amplitude : amplitude * probeDrop;
        float input = 0.0f;
        for (const auto frequency : frequencies)
            input += level * std::sin(
                2.0f * werfeed::pi * frequency * i / 48000.0f);
        processSample(processor, input);
    }
}

} // namespace

int main() {
    constexpr double rate = 48000.0;
    static_assert(werfeed::sharedNotchCapacity(32, 4, 0) == 8);
    static_assert(werfeed::sharedNotchCapacity(32, 3, 0) == 11);
    static_assert(werfeed::sharedNotchCapacity(32, 2, 0) == 16);
    static_assert(werfeed::sharedNotchCapacity(32, 1, 0) == 32);
    static_assert(werfeed::sharedNotchCapacity(32, 0, 0) == 0);
    static_assert(werfeed::sharedNotchCapacity(48, 8, 7) == 6);
    static_assert(werfeed::sharedNotchCapacity(48, 5, 0) == 10);
    static_assert(werfeed::sharedNotchCapacity(48, 5, 4) == 9);
    const auto sweep = werfeed::makeLogSweep(rate, 1.0);
    REQUIRE(sweep.size() == 48000);
    REQUIRE(*std::max_element(sweep.begin(), sweep.end()) <= 0.081f);

    std::vector<float> impulse(256, 0.0f), delayed(512, 0.0f);
    impulse[0] = 0.08f;
    delayed[137] = 0.08f;
    REQUIRE(werfeed::estimateDelay(impulse, delayed, 240) == 137);
    for (std::size_t i = 138; i < 190; ++i)
        delayed[i] = 0.025f * std::exp(-static_cast<float>(i - 138) / 18.0f);
    for (std::size_t i = 0; i < delayed.size(); ++i)
        delayed[i] += 0.001f * std::sin(static_cast<float>(i) * 1.731f);
    REQUIRE(werfeed::estimateDelay(std::span<const float>(impulse.data(), 64), delayed, 240, 0.2f) == 137);
    std::fill(delayed.begin(), delayed.end(), 0.0f);
    REQUIRE(werfeed::estimateDelay(impulse, delayed, 240) == -1);
    const auto probe = werfeed::makeDelayProbe();
    std::vector<float> noiseOnly(26000);
    unsigned noiseState = 17;
    for (auto& value : noiseOnly) {
        noiseState = noiseState * 1664525u + 1013904223u;
        value = (static_cast<float>((noiseState >> 8u) & 0xffffu) / 32768.0f - 1.0f) * 0.01f;
    }
    // Silence/noise below the correlation threshold must not produce a delay.
    REQUIRE(werfeed::estimateDelay(probe, noiseOnly, 24000, 0.2f) == -1);

    std::vector<float> response(sweep.size() + 128, 0.0f);
    for (std::size_t i = 0; i < sweep.size(); ++i) response[i + 128] = sweep[i] * 0.5f;
    const auto measured = werfeed::measureResponse(sweep, response, rate, 128);
    // Flat gain is shifted to the -3 dB median calibration reference.
    const auto normalizedMeasured = werfeed::normalizeCalibrationResponse(measured);
    for (const auto db : normalizedMeasured)
        REQUIRE(std::isfinite(db) && std::abs(db + 3.0f) < 1.5f);

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
    const auto normalizedShaped = werfeed::normalizeCalibrationResponse(shapedMeasured);
    auto sortedShaped = shapedMeasured;
    std::sort(sortedShaped.begin(), sortedShaped.end());
    const auto shapedMedian = 0.5f * (sortedShaped[(sortedShaped.size() - 1) / 2] +
                                      sortedShaped[sortedShaped.size() / 2]);
    const auto expectedOffset = -3.0f - shapedMedian;
    auto sortedNormalized = normalizedShaped;
    std::sort(sortedNormalized.begin(), sortedNormalized.end());
    const auto normalizedMedian = 0.5f * (sortedNormalized[(sortedNormalized.size() - 1) / 2] +
                                          sortedNormalized[sortedNormalized.size() / 2]);
    REQUIRE(std::abs(normalizedMedian + 3.0f) < 0.05f);
    // The normalization uses one scalar offset across the whole spectrum.
    const auto wholeCurveOffset = normalizedShaped.front() - shapedMeasured.front();
    REQUIRE(std::abs(wholeCurveOffset - expectedOffset) < 0.001f);
    for (std::size_t bin = 0; bin < werfeed::analyzerBins; ++bin)
        REQUIRE(std::abs((normalizedShaped[bin] - shapedMeasured[bin]) -
                         wholeCurveOffset) < 0.001f);
    // The 40 ms local analysis window averages a small portion of the sweep,
    // so the normalized curve must follow the relative response within 1.0 dB.
    for (std::size_t bin = 0; bin < werfeed::analyzerBins; ++bin) {
        const auto position = static_cast<double>(bin) /
            static_cast<double>(werfeed::analyzerBins - 1);
        const auto frequency = calibrationStartHz *
            std::pow(calibrationEndHz / calibrationStartHz, position);
        REQUIRE(std::isfinite(shapedMeasured[bin]));
        REQUIRE(std::abs(normalizedShaped[bin] -
                         static_cast<float>(shapedRoomResponseDb(frequency) + expectedOffset)) < 1.0f);
    }
    std::array<float, werfeed::analyzerBins> calibrationPeaks {};
    calibrationPeaks.fill(0.0f);
    calibrationPeaks[96] = 12.0f;
    const auto calibratedBaseline = werfeed::detectionBaseline(calibrationPeaks);
    const auto calibratedBias = werfeed::calibrationBiasFromResponse(calibrationPeaks);
    REQUIRE(calibratedBaseline[96] < calibratedBaseline[95]);
    REQUIRE(calibratedBaseline[96] <= -61.0f);
    REQUIRE(calibratedBaseline[20] == -55.0f);
    REQUIRE(calibratedBias[96] >= 6.0f);
    REQUIRE(calibratedBias[95] > 0.0f);
    REQUIRE(calibratedBias[94] > 0.0f);
    REQUIRE(calibratedBias[93] == 0.0f);
    REQUIRE(std::abs(werfeed::notchQuality(100.0f) - 9.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::notchQuality(500.0f) - 11.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::notchQuality(1000.0f) - 14.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::notchQuality(4000.0f) - 20.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::notchQuality(10000.0f) - 30.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::notchQualityForPreset(
        500.0f, werfeed::ProtectionPreset::speech) - 11.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::notchQualityForPreset(
        500.0f, werfeed::ProtectionPreset::music) - 10.12f) < 0.01f);
    REQUIRE(werfeed::notchQualityForCalibrationHotspot(
        500.0f, werfeed::ProtectionPreset::speech, true) <
        werfeed::notchQualityForPreset(500.0f, werfeed::ProtectionPreset::speech));
    REQUIRE(std::abs(werfeed::notchQualityForCalibrationHotspot(
        1000.0f, werfeed::ProtectionPreset::speech, true) -
        werfeed::notchQualityForPreset(1000.0f, werfeed::ProtectionPreset::speech)) < 0.01f);
    REQUIRE(std::abs(werfeed::probeDepthFraction(299.0f) - 0.5f) < 0.001f);
    REQUIRE(std::abs(werfeed::probeDepthFraction(300.0f) - 0.75f) < 0.001f);
    REQUIRE(std::abs(werfeed::probeDepthFraction(800.0f) - 0.75f) < 0.001f);
    REQUIRE(std::abs(werfeed::probeDepthFraction(801.0f) - 0.5f) < 0.001f);

    auto separatedControls = std::make_unique<werfeed::FeedbackProcessor>();
    separatedControls->prepare(rate);
    separatedControls->setDepthAmount(0.4f);
    separatedControls->setSensitivityAmount(0.9f);
    const auto separatedSnapshot = separatedControls->snapshot();
    REQUIRE(std::abs(separatedSnapshot.depthAmount - 0.4f) < 0.001f);
    REQUIRE(std::abs(separatedSnapshot.sensitivityAmount - 0.9f) < 0.001f);

    auto immediateHotspot = std::make_unique<werfeed::FeedbackProcessor>();
    immediateHotspot->prepare(rate);
    immediateHotspot->setEnabled(true);
    immediateHotspot->setSuppressionAmount(1.0f);
    std::array<float, werfeed::analyzerBins> immediateHotspotResponse {};
    immediateHotspotResponse.fill(0.0f);
    constexpr std::size_t immediateHotspotBin = 120;
    const auto immediateHotspotFrequency = static_cast<float>(
        20.0 * std::pow(1000.0,
            static_cast<double>(immediateHotspotBin) /
            static_cast<double>(werfeed::analyzerBins - 1)));
    immediateHotspotResponse[immediateHotspotBin] = 12.0f;
    immediateHotspot->setCalibrationProfile(immediateHotspotResponse);
    for (int i = 0; i < 96000; ++i)
        processSample(*immediateHotspot, 0.3f * std::sin(
            2.0f * werfeed::pi * immediateHotspotFrequency * i / 48000.0f));
    const auto immediateHotspotSnapshot = immediateHotspot->snapshot();
    const auto immediateHotspotNotch = *std::min_element(
        immediateHotspotSnapshot.notches.begin(), immediateHotspotSnapshot.notches.end(),
        [immediateHotspotFrequency](const werfeed::NotchSnapshot& a,
                                    const werfeed::NotchSnapshot& b) {
            const auto aDistance = a.active
                ? std::abs(std::log2(a.frequency / immediateHotspotFrequency)) : 1000.0f;
            const auto bDistance = b.active
                ? std::abs(std::log2(b.frequency / immediateHotspotFrequency)) : 1000.0f;
            return aDistance < bDistance;
        });
    REQUIRE(immediateHotspotNotch.active);
    REQUIRE(immediateHotspotNotch.depthDb < -24.0f);

    auto ordinaryTone = std::make_unique<werfeed::FeedbackProcessor>();
    ordinaryTone->prepare(rate);
    ordinaryTone->setEnabled(true);
    ordinaryTone->setSuppressionAmount(1.0f);
    std::array<float, werfeed::analyzerBins> baseline {};
    baseline.fill(-80.0f);
    ordinaryTone->setBaseline(baseline);
    float previous = 0.0f;
    for (int i = 0; i < 48000; ++i) {
        const auto input = 0.3f * std::sin(2.0f * werfeed::pi * 1000.0f * i / 48000.0f);
        const auto output = processSample(*ordinaryTone, input);
        REQUIRE(std::isfinite(output));
        REQUIRE(std::abs(output - previous) < 0.5f); // coefficient ramp remains click-free.
        previous = output;
    }
    REQUIRE(ordinaryTone->snapshot().activeNotches == 0);

    auto confirmedFeedback = std::make_unique<werfeed::FeedbackProcessor>();
    confirmedFeedback->prepare(rate);
    confirmedFeedback->setEnabled(true);
    confirmedFeedback->setSuppressionAmount(1.0f);
    confirmedFeedback->setSensitivityAmount(1.0f);
    confirmedFeedback->setBaseline(baseline);
    processConfirmedTone(*confirmedFeedback, 1000.0f, 0.3f, 48000);
    const auto snapshot = confirmedFeedback->snapshot();
    REQUIRE(snapshot.activeNotches > 0);
    REQUIRE(snapshot.activeNotches <= static_cast<int>(werfeed::maxNotches));
    REQUIRE(snapshot.maximumCutDb >= -38.5f && snapshot.maximumCutDb <= -37.8f);
    const auto active = *std::min_element(snapshot.notches.begin(), snapshot.notches.end(),
        [](const werfeed::NotchSnapshot& a, const werfeed::NotchSnapshot& b) {
            const auto aDistance = a.active ? std::abs(std::log2(a.frequency / 1000.0f)) : 1000.0f;
            const auto bDistance = b.active ? std::abs(std::log2(b.frequency / 1000.0f)) : 1000.0f;
            return aDistance < bDistance;
        });
    double inputPower = 0.0, outputPower = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const auto input = 0.3f * std::sin(2.0f * werfeed::pi * active.frequency * i / 48000.0f);
        const auto output = processSample(*confirmedFeedback, input);
        if (i > 24000) { inputPower += input * input; outputPower += output * output; }
    }
    REQUIRE(10.0 * std::log10(outputPower / inputPower) < -2.0);
    for (const auto frequency : { 997.0f, 8000.0f, 15731.0f }) {
        auto offGrid = std::make_unique<werfeed::FeedbackProcessor>();
        offGrid->prepare(rate); offGrid->clearBaseline(); offGrid->setEnabled(true);
        processConfirmedTone(*offGrid, frequency, 0.3f);
        const auto offGridSnapshot = offGrid->snapshot();
        REQUIRE(offGridSnapshot.activeNotches > 0);
        REQUIRE(offGridSnapshot.maximumCutDb < -6.0f);
        REQUIRE(std::any_of(offGridSnapshot.notches.begin(), offGridSnapshot.notches.end(),
            [frequency](const werfeed::NotchSnapshot& notch) {
                 return notch.active && std::abs(notch.frequency - frequency) /
                     frequency < 0.02f;
            }));
    }
    // A lower-level feedback tone should engage below the old 12 dB speech
    // gate while still passing the tonal and persistence checks.
    auto quietFeedback = std::make_unique<werfeed::FeedbackProcessor>();
    quietFeedback->prepare(rate); quietFeedback->setEnabled(true);
    quietFeedback->setSensitivityAmount(1.0f);
    int firstEngagedSample = -1;
    for (int i = 0; i < 144000; ++i) {
        const auto level = i < 4096 ? 0.03f : 0.03f *
            std::pow(10.0f, werfeed::maximumSuppressionDepth(
                quietFeedback->getSuppressionAmount()) * 0.5f / 20.0f);
        processSample(*quietFeedback, level * std::sin(
            2.0f * werfeed::pi * 1000.0f * i / 48000.0f));
        if (firstEngagedSample < 0 && quietFeedback->snapshot().activeNotches > 0)
            firstEngagedSample = i;
    }
    REQUIRE(firstEngagedSample >= 0 && firstEngagedSample < 12000);
    REQUIRE(quietFeedback->snapshot().activeNotches > 0);
    // Notch release is generic behavior, not a special case for 70 Hz:
    // exercise it at two representative low-frequency tones.
    for (const auto frequency : { 70.0f }) {
        auto releaseTone = std::make_unique<werfeed::FeedbackProcessor>();
        releaseTone->prepare(rate); releaseTone->clearBaseline(); releaseTone->setEnabled(true);
        releaseTone->setSuppressionAmount(1.0f);
        processConfirmedTone(*releaseTone, frequency, 0.3f, 14000);
        const auto releaseSnapshot = releaseTone->snapshot();
        REQUIRE(releaseSnapshot.activeNotches == 1);
        REQUIRE(releaseSnapshot.maximumCutDb <= -37.8f);
        REQUIRE(std::any_of(releaseSnapshot.notches.begin(), releaseSnapshot.notches.end(),
            [frequency](const werfeed::NotchSnapshot& notch) {
                return notch.active && std::abs(notch.frequency - frequency) / frequency < 0.02f
                    && notch.q < 10.0f;
            }));
        for (int i = 0; i < 96000; ++i) processSample(*releaseTone, 0.0f);
        REQUIRE(releaseTone->snapshot().activeNotches == 1);
        for (int i = 0; i < 300000; ++i) processSample(*releaseTone, 0.0f);
        REQUIRE(releaseTone->snapshot().activeNotches == 0);
    }
    // The speech scale reaches -14 dB at 70%, -24 dB at 80%, and -32 dB at full depth.
    auto seventyPercent = std::make_unique<werfeed::FeedbackProcessor>();
    seventyPercent->prepare(rate); seventyPercent->setBaseline(baseline);
    seventyPercent->setEnabled(true); seventyPercent->setSuppressionAmount(0.7f);
    processConfirmedTone(*seventyPercent, 1000.0f, 0.3f, 96000);
    const auto seventySnapshot = seventyPercent->snapshot();
    REQUIRE(seventySnapshot.maximumCutDb >= -14.1f &&
            seventySnapshot.maximumCutDb <= -13.4f);
    seventyPercent->setSuppressionAmount(0.8f);
    processConfirmedTone(*seventyPercent, 1000.0f, 0.3f, 96000);
    REQUIRE(seventyPercent->snapshot().maximumCutDb >= -24.1f &&
            seventyPercent->snapshot().maximumCutDb <= -23.4f);
    // The upper 20% of Speech lowers the threshold beyond the former maximum.
    auto thresholdAtSeventy = std::make_unique<werfeed::FeedbackProcessor>();
    thresholdAtSeventy->prepare(rate); thresholdAtSeventy->clearBaseline();
    thresholdAtSeventy->setEnabled(true); thresholdAtSeventy->setSuppressionAmount(0.7f);
    thresholdAtSeventy->setSensitivityAmount(0.7f);
    auto thresholdAtHundred = std::make_unique<werfeed::FeedbackProcessor>();
    thresholdAtHundred->prepare(rate); thresholdAtHundred->clearBaseline();
    thresholdAtHundred->setEnabled(true); thresholdAtHundred->setSuppressionAmount(1.0f);
    thresholdAtHundred->setSensitivityAmount(1.0f);
    for (int i = 0; i < 16000; ++i) {
        const auto borderlineTone = 0.02f * std::sin(
            2.0f * werfeed::pi * 1000.0f * i / 48000.0f);
        processSample(*thresholdAtSeventy, borderlineTone);
        const auto confirmedLevel = i < 4096 ? 0.02f : 0.02f *
            std::pow(10.0f, werfeed::maximumSuppressionDepth(
                thresholdAtHundred->getSuppressionAmount()) * 0.5f / 20.0f);
        processSample(*thresholdAtHundred, confirmedLevel * std::sin(
            2.0f * werfeed::pi * 1000.0f * i / 48000.0f));
    }
    REQUIRE(thresholdAtSeventy->snapshot().activeNotches == 0);
    REQUIRE(thresholdAtHundred->snapshot().activeNotches > 0);

    // Above 1 kHz, the same borderline tone should cross the lower
    // frequency-weighted gate sooner.
    auto lowFrequency = std::make_unique<werfeed::FeedbackProcessor>();
    lowFrequency->prepare(rate); lowFrequency->clearBaseline();
    lowFrequency->setEnabled(true); lowFrequency->setSuppressionAmount(0.5f);
    auto highFrequency = std::make_unique<werfeed::FeedbackProcessor>();
    highFrequency->prepare(rate); highFrequency->clearBaseline();
    highFrequency->setEnabled(true); highFrequency->setSuppressionAmount(0.5f);
    highFrequency->setSensitivityAmount(1.0f);
    processConfirmedTone(*lowFrequency, 1000.0f, 0.0025f, 96000);
    processConfirmedTone(*highFrequency, 4000.0f, 0.02f, 96000);
    REQUIRE(lowFrequency->snapshot().activeNotches == 0);
    REQUIRE(highFrequency->snapshot().activeNotches > 0);

    // A manual cut remains active through silence until the explicit clear.
    auto manualCut = std::make_unique<werfeed::FeedbackProcessor>();
    manualCut->prepare(rate); manualCut->setEnabled(true); manualCut->setSuppressionAmount(0.8f);
    manualCut->setLatchAmount(0.0f);
    manualCut->setManualNotch(1500.0f);
    for (int i = 0; i < 96000; ++i) processSample(*manualCut, 0.0f);
    REQUIRE(manualCut->snapshot().activeNotches == 1);
    for (int i = 0; i < 192000; ++i) processSample(*manualCut, 0.0f);
    REQUIRE(manualCut->snapshot().activeNotches == 1);
    manualCut->clearManualNotch(1500.0f);
    for (int i = 0; i < 24000; ++i) processSample(*manualCut, 0.0f);
    REQUIRE(manualCut->snapshot().activeNotches == 0);
    REQUIRE(werfeed::persistentNotchLimit(8, 0.0f) == 0);
    REQUIRE(werfeed::persistentNotchLimit(8, 0.5f) == 2);
    REQUIRE(werfeed::persistentNotchLimit(8, 1.0f) == 5);
    REQUIRE(werfeed::maximumSuppressionDepth(1.0f) >= -38.5f &&
            werfeed::maximumSuppressionDepth(1.0f) <= -37.8f);
    REQUIRE(std::abs(werfeed::feedbackAmplitudeThresholdDb(0.0f) - 0.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::feedbackAmplitudeThresholdDb(1.0f) - (-70.0f)) < 0.01f);
    REQUIRE(std::abs(werfeed::detectorSensitivityAmount(0.6f, false) - 0.6f) < 0.01f);
    REQUIRE(std::abs(werfeed::calibrationAmplitudeExcessDb(1.5f) - 3.2f) < 0.01f);
    REQUIRE(std::abs(werfeed::calibrationAmplitudeExcessDb(8.0f) - 8.4f) < 0.01f);
    REQUIRE(std::abs(werfeed::detectorHotspotSensitivityLift(1.5f) - 0.2f) < 0.01f);
    REQUIRE(werfeed::detectorHotspotSensitivityLift(8.0f) >
            werfeed::detectorHotspotSensitivityLift(1.5f));
    REQUIRE(std::abs(werfeed::detectorHotspotSensitivityLift(16.0f) - 0.5f) < 0.01f);
    REQUIRE(std::abs(werfeed::detectorSensitivityAmount(0.6f, true, 1.5f) - 0.8f) < 0.01f);
    REQUIRE(werfeed::detectorSensitivityAmount(0.6f, true, 8.0f) >
            werfeed::detectorSensitivityAmount(0.6f, true, 1.5f));
    REQUIRE(std::abs(werfeed::frequencyThresholdAdjustmentDb(20.0f) - 10.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::frequencyThresholdAdjustmentDb(150.0f) - 10.0f) < 0.01f);
    REQUIRE(werfeed::frequencyThresholdAdjustmentDb(200.0f) >
            werfeed::frequencyThresholdAdjustmentDb(500.0f));
    REQUIRE(std::abs(werfeed::frequencyThresholdAdjustmentDb(500.0f)) < 0.01f);
    REQUIRE(std::abs(werfeed::frequencyThresholdAdjustmentDb(1500.0f)) < 0.01f);
    REQUIRE(werfeed::frequencyThresholdAdjustmentDb(4000.0f) <
            werfeed::frequencyThresholdAdjustmentDb(1500.0f));
    REQUIRE(std::abs(werfeed::frequencyThresholdAdjustmentDb(8000.0f) + 18.0f) < 0.01f);
    REQUIRE(std::abs(werfeed::frequencyThresholdAdjustmentDb(20000.0f) + 18.0f) < 0.01f);
    const auto ordinaryLowGate = werfeed::detectorEngageThresholdDb(
        0.1f, werfeed::ProtectionPreset::speech, false, 0.0f, 100.0f);
    const auto hotspotLowGate = werfeed::detectorEngageThresholdDb(
        0.1f, werfeed::ProtectionPreset::speech, true, 1.5f, 100.0f);
    const auto hotspotHighGate = werfeed::detectorEngageThresholdDb(
        0.1f, werfeed::ProtectionPreset::speech, true, 1.5f, 4000.0f);
    REQUIRE(hotspotLowGate < ordinaryLowGate);
    REQUIRE(hotspotHighGate < hotspotLowGate);
    REQUIRE(werfeed::detectorAmplitudeThresholdDb(0.1f, true, 4000.0f) <
            werfeed::detectorAmplitudeThresholdDb(0.1f, true, 100.0f));
    REQUIRE(werfeed::detectorAmplitudeThresholdDb(0.1f, true, 1000.0f, 8.0f) <
            werfeed::detectorAmplitudeThresholdDb(0.1f, true, 1000.0f, 1.5f));
    REQUIRE(werfeed::persistentRecurrenceWindowFrames(0.79f) == 420);
    REQUIRE(werfeed::persistentRecurrenceWindowFrames(0.8f) == 480);
    REQUIRE(werfeed::persistentRecurrenceWindowFrames(1.0f) == 520);
    REQUIRE(werfeed::persistentRecurrenceRequirement(0.79f) == 6);
    REQUIRE(werfeed::persistentRecurrenceRequirement(0.8f) == 3);
    REQUIRE(werfeed::persistentMinimumProbeDepthDb(0.79f) == 12.0f);
    REQUIRE(werfeed::persistentMinimumProbeDepthDb(0.8f) == 10.0f);

    // A calibrated resonance remains protected longer than an ordinary tone.
    constexpr float hotspotFrequency = 1000.0f;
    const auto hotspotBin = static_cast<std::size_t>(std::lround(
        std::log10(hotspotFrequency / 20.0f) / std::log10(1000.0f) *
        static_cast<float>(werfeed::analyzerBins - 1)));
    std::array<float, werfeed::analyzerBins> hotspotResponse {};
    hotspotResponse.fill(0.0f);
    hotspotResponse[hotspotBin] = 14.0f;
    auto calibratedHotspot = std::make_unique<werfeed::FeedbackProcessor>();
    calibratedHotspot->prepare(rate);
    calibratedHotspot->setCalibrationProfile(hotspotResponse);
    calibratedHotspot->setEnabled(true);
    calibratedHotspot->setSuppressionAmount(1.0f);
    calibratedHotspot->setTimingAmount(0.0f);
    calibratedHotspot->setLatchAmount(0.0f);
    processConfirmedTone(*calibratedHotspot, hotspotFrequency, 0.04f, 14000);
    REQUIRE(calibratedHotspot->snapshot().activeNotches > 0);
    for (int i = 0; i < 600000; ++i) processSample(*calibratedHotspot, 0.0f);
    REQUIRE(calibratedHotspot->snapshot().activeNotches == 0);

    // A held hotspot stays latched while the latch is open, but moving the
    // latch to zero releases it promptly when the feedback is no longer present.
    auto releasableHeldHotspot = std::make_unique<werfeed::FeedbackProcessor>();
    releasableHeldHotspot->prepare(rate);
    releasableHeldHotspot->setCalibrationProfile(hotspotResponse);
    releasableHeldHotspot->setEnabled(true);
    releasableHeldHotspot->setSuppressionAmount(1.0f);
    releasableHeldHotspot->setTimingAmount(0.0f);
    releasableHeldHotspot->setLatchAmount(1.0f);
    for (int burst = 0; burst < 6; ++burst) {
        for (int i = 0; i < 12000; ++i)
            processSample(*releasableHeldHotspot, 0.04f * std::sin(
                2.0f * werfeed::pi * hotspotFrequency * i / 48000.0f));
        for (int i = 0; i < 3000; ++i)
            processSample(*releasableHeldHotspot, 0.0f);
    }
    for (int i = 0; i < 24000; ++i)
        processSample(*releasableHeldHotspot, 0.0f);
    REQUIRE(releasableHeldHotspot->snapshot().activeNotches > 0);
    releasableHeldHotspot->setLatchAmount(0.0f);
    for (int i = 0; i < 240000; ++i)
        processSample(*releasableHeldHotspot, 0.0f);
    REQUIRE(releasableHeldHotspot->snapshot().activeNotches == 0);

    // Automatic persistence requires a measured hotspot. Repeating an
    // uncalibrated tone must still release even when the latch is wide open.
    auto uncalibratedRepeat = std::make_unique<werfeed::FeedbackProcessor>();
    uncalibratedRepeat->prepare(rate);
    uncalibratedRepeat->setEnabled(true);
    uncalibratedRepeat->setSuppressionAmount(1.0f);
    uncalibratedRepeat->setTimingAmount(0.0f);
    uncalibratedRepeat->setLatchAmount(1.0f);
    processConfirmedTone(*uncalibratedRepeat, hotspotFrequency, 0.04f, 14000);
    for (int i = 0; i < 960000; ++i) processSample(*uncalibratedRepeat, 0.0f);
    REQUIRE(uncalibratedRepeat->snapshot().activeNotches == 0);

    // Repeated shallow bursts still require a deep intervention before they
    // can become persistent, even when recurrence and latch capacity qualify.
    auto calibratedRepeat = std::make_unique<werfeed::FeedbackProcessor>();
    calibratedRepeat->prepare(rate);
    calibratedRepeat->setCalibrationProfile(hotspotResponse);
    calibratedRepeat->setEnabled(true);
    calibratedRepeat->setSuppressionAmount(0.7f);
    calibratedRepeat->setTimingAmount(0.0f);
    calibratedRepeat->setLatchAmount(1.0f);
    for (int burst = 0; burst < 4; ++burst) {
        for (int i = 0; i < 24000; ++i)
            processSample(*calibratedRepeat, 0.04f * std::sin(
                2.0f * werfeed::pi * hotspotFrequency * i / 48000.0f));
        for (int i = 0; i < 12000; ++i) processSample(*calibratedRepeat, 0.0f);
    }
    for (int i = 0; i < 960000; ++i) processSample(*calibratedRepeat, 0.0f);
    REQUIRE(calibratedRepeat->snapshot().activeNotches == 0);
    auto twoTone = std::make_unique<werfeed::FeedbackProcessor>();
    twoTone->prepare(rate); twoTone->clearBaseline(); twoTone->setEnabled(true);
    processConfirmedTones(*twoTone,
        std::array<float, 2> { 984.375f, 3000.0f }, 0.2f);
    const auto twoToneSnapshot = twoTone->snapshot();
    REQUIRE(std::count_if(twoToneSnapshot.notches.begin(), twoToneSnapshot.notches.end(),
        [](const werfeed::NotchSnapshot& notch) { return notch.active; }) >= 2);

    // Adjacent low-mid peaks that need their own narrow cuts should be
    // collapsed into one weighted-center cut with a wider band and slightly
    // deeper depth.
    auto coupledLowMid = std::make_unique<werfeed::FeedbackProcessor>();
    coupledLowMid->prepare(rate); coupledLowMid->clearBaseline();
    coupledLowMid->setEnabled(true); coupledLowMid->setSuppressionAmount(1.0f);
    constexpr std::array<float, 3> coupledFrequencies {
        175.78125f, 199.21875f, 222.65625f,
    };
    processConfirmedTones(*coupledLowMid, coupledFrequencies, 0.3f);
    const auto coupledSnapshot = coupledLowMid->snapshot();
    REQUIRE(coupledLowMid->getNotchCapacity() == werfeed::defaultNotchesPerRoute);
    REQUIRE(std::count_if(coupledSnapshot.notches.begin(), coupledSnapshot.notches.end(),
        [](const werfeed::NotchSnapshot& notch) { return notch.active; }) <=
        static_cast<int>(werfeed::defaultNotchesPerRoute));

    // A route that receives released capacity can hold more than the normal
    // eight simultaneous cuts without allocating on the realtime thread.
    auto expanded = std::make_unique<werfeed::FeedbackProcessor>();
    expanded->prepare(rate); expanded->clearBaseline(); expanded->setEnabled(true);
    expanded->setNotchCapacity(8);
    constexpr std::array<float, 8> expandedFrequencies {
        187.5f, 375.0f, 750.0f, 1500.0f,
        3000.0f, 6000.0f, 9000.0f, 12000.0f,
    };
    processConfirmedTones(*expanded, expandedFrequencies, 0.035f);
    const auto expandedSnapshot = expanded->snapshot();
    REQUIRE(expanded->getNotchCapacity() == 8);
    REQUIRE(expandedSnapshot.activeNotches <= 8);

    // A live route re-arm must not expose the previous route's cuts while
    // capacity is being redistributed.
    expanded->setNotchCapacity(12);
    expanded->requestNotchReset();
    expanded->pullPendingNotchUpdates();
    expanded->pumpBackgroundAnalysis();
    const auto rearmedSnapshot = expanded->snapshot();
    REQUIRE(expanded->getNotchCapacity() == 12);
    REQUIRE(rearmedSnapshot.activeNotches == 0);

    auto limited = std::make_unique<werfeed::FeedbackProcessor>();
    limited->prepare(rate); limited->clearBaseline(); limited->setEnabled(true);
    limited->setNotchCapacity(1);
    processConfirmedTones(*limited, std::array<float, 2> { 750.0f, 3000.0f }, 0.15f);
    REQUIRE(limited->snapshot().activeNotches <= 1);

    // Bypass transitions remain continuous while wet filter history advances.
    float last = 0.0f;
    for (int i = 0; i < 12000; ++i) {
        if (i == 3000) confirmedFeedback->setEnabled(false);
        if (i == 7000) confirmedFeedback->setEnabled(true);
        const auto input = 0.25f * std::sin(2.0f * werfeed::pi * 1000.0f * i / 48000.0f);
        const auto output = processSample(*confirmedFeedback, input);
        REQUIRE(std::isfinite(output));
        if (i > 0) REQUIRE(std::abs(output - last) < 0.2f);
        last = output;
    }

    // Short, moving tonal program peaks do not satisfy persistence.
    auto program = std::make_unique<werfeed::FeedbackProcessor>();
    program->prepare(rate); program->clearBaseline(); program->setPreset(werfeed::ProtectionPreset::music);
    program->setEnabled(true);
    constexpr std::array<float, 8> notes { 220.0f, 329.63f, 440.0f, 587.33f,
                                           261.63f, 392.0f, 523.25f, 783.99f };
    for (int i = 0; i < 96000; ++i) {
        const auto note = notes[static_cast<std::size_t>(i / 3840) % notes.size()];
        const auto envelope = std::sin(werfeed::pi * static_cast<float>(i % 3840) / 3840.0f);
        const auto input = 0.22f * envelope * std::sin(2.0f * werfeed::pi * note * i / 48000.0f);
        processSample(*program, input);
    }
    REQUIRE(program->snapshot().activeNotches == 0);

    // Speech may start its shallow probe on the first qualifying analysis
    // frame, while Music waits for a longer stable peak.
    auto speechTiming = std::make_unique<werfeed::FeedbackProcessor>();
    speechTiming->prepare(rate);
    speechTiming->clearBaseline();
    speechTiming->setEnabled(true);
    speechTiming->setPreset(werfeed::ProtectionPreset::speech);
    auto musicTiming = std::make_unique<werfeed::FeedbackProcessor>();
    musicTiming->prepare(rate);
    musicTiming->clearBaseline();
    musicTiming->setEnabled(true);
    musicTiming->setPreset(werfeed::ProtectionPreset::music);
    int speechProbeSample = -1;
    int musicProbeSample = -1;
    for (int i = 0; i < 24000; ++i) {
        const auto tone = 0.3f * std::sin(
            2.0f * werfeed::pi * 1000.0f * i / 48000.0f);
        processSample(*speechTiming, tone);
        processSample(*musicTiming, tone);
        if (speechProbeSample < 0 && speechTiming->snapshot().activeNotches > 0)
            speechProbeSample = i;
        if (musicProbeSample < 0 && musicTiming->snapshot().activeNotches > 0)
            musicProbeSample = i;
    }
    REQUIRE(speechProbeSample >= 0);
    REQUIRE(musicProbeSample > speechProbeSample);

    // A malformed input sample must never propagate a non-finite output.
    REQUIRE(std::isfinite(processSample(*program, std::numeric_limits<float>::quiet_NaN())));
    REQUIRE(std::isfinite(processSample(*program, std::numeric_limits<float>::infinity())));
    return 0;
}