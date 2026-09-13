#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace werfeed {

constexpr float pi = 3.14159265358979323846f;
constexpr std::size_t analyzerBins = 256;
constexpr std::size_t maxNotches = 48;
constexpr std::size_t defaultNotchesPerRoute = 8;

constexpr std::size_t sharedNotchCapacity(std::size_t totalSlots,
                                          std::size_t activeRoutes,
                                          std::size_t activeOrdinal) noexcept {
    if (activeRoutes == 0 || activeOrdinal >= activeRoutes) return 0;
    const auto boundedSlots = std::min(maxNotches, totalSlots);
    const auto base = boundedSlots / activeRoutes;
    const auto remainder = boundedSlots % activeRoutes;
    return base + (activeOrdinal < remainder ? 1 : 0);
}

enum class ProtectionPreset { speech, music };

struct NotchSnapshot {
    float frequency = 0.0f;
    float depthDb = 0.0f;
    float q = 0.0f;
    bool active = false;
};

struct ProtectionSnapshot {
    std::array<float, analyzerBins> spectrumDb {};
    std::array<NotchSnapshot, maxNotches> notches {};
    int activeNotches = 0;
    float maximumCutDb = 0.0f;
    float depthAmount = 0.75f;
    float sensitivityAmount = 0.75f;
    // Compatibility alias for older telemetry consumers.
    float suppressionAmount = 0.75f;
    float timingAmount = 0.5f;
    float latchAmount = 0.5f;
};

inline std::vector<float> makeLogSweep(double sampleRate, double seconds,
                                       float startHz = 20.0f, float endHz = 20000.0f,
                                       float amplitude = 0.08f) {
    const auto count = static_cast<std::size_t>(std::max(1.0, sampleRate * seconds));
    std::vector<float> result(count);
    const auto ratio = static_cast<double>(endHz / startHz);
    const auto duration = static_cast<double>(count) / sampleRate;
    const auto scale = 2.0 * static_cast<double>(pi) * startHz * duration / std::log(ratio);
    for (std::size_t i = 0; i < count; ++i) {
        const auto t = static_cast<double>(i) / sampleRate;
        const auto phase = scale * (std::pow(ratio, t / duration) - 1.0);
        const auto fade = std::min({1.0, t / 0.02, (duration - t) / 0.02});
        result[i] = amplitude * static_cast<float>(std::max(0.0, fade) * std::sin(phase));
    }
    return result;
}

inline std::vector<float> makeDelayProbe(float amplitude = 0.06f, std::size_t length = 2047) {
    std::vector<float> result(length);
    unsigned state = 0x5a3u;
    for (std::size_t i = 0; i < length; ++i) {
        const auto bit = ((state >> 10u) ^ (state >> 8u)) & 1u;
        state = ((state << 1u) | bit) & 0x7ffu;
        result[i] = (state & 1u) ? amplitude : -amplitude;
    }
    return result;
}

inline int estimateDelay(std::span<const float> emitted, std::span<const float> recorded,
                         int maximumDelaySamples, float minimumCorrelation = 0.25f) {
    if (emitted.empty() || recorded.empty() || maximumDelaySamples < 0) return -1;
    float best = -1.0f;
    int bestDelay = -1;
    const auto limit = std::min<int>(maximumDelaySamples,
        static_cast<int>(recorded.size()) - 1);
    for (int delay = 0; delay <= limit; ++delay) {
        double dot = 0.0, sourcePower = 0.0, recordedPower = 0.0;
        const auto count = std::min(emitted.size(), recorded.size() - static_cast<std::size_t>(delay));
        for (std::size_t i = 0; i < count; ++i) {
            const auto a = emitted[i];
            const auto b = recorded[i + static_cast<std::size_t>(delay)];
            dot += a * b; sourcePower += a * a; recordedPower += b * b;
        }
        const auto denominator = std::sqrt(sourcePower * recordedPower);
        const auto correlation = denominator > 1.0e-12 ? static_cast<float>(dot / denominator) : 0.0f;
        if (correlation > best) { best = correlation; bestDelay = delay; }
    }
    return best >= minimumCorrelation ? bestDelay : -1;
}

inline std::array<float, analyzerBins> measureResponse(
    std::span<const float> excitation, std::span<const float> response,
    double sampleRate, int delaySamples = 0) {
    std::array<float, analyzerBins> result {};
    const auto usable = delaySamples >= 0 && static_cast<std::size_t>(delaySamples) < response.size()
        ? response.subspan(static_cast<std::size_t>(delaySamples)) : std::span<const float> {};
    const auto count = std::min(excitation.size(), usable.size());
    if (count < 16 || sampleRate <= 0.0) {
        result.fill(0.0f);
        return result;
    }
    constexpr float startHz = 20.0f;
    constexpr float endHz = 20000.0f;
    const auto duration = static_cast<double>(count) / sampleRate;
    const auto ratio = static_cast<double>(endHz / startHz);
    const auto logRatio = std::log(ratio);
    const auto sweepScale = 2.0 * static_cast<double>(pi) * startHz * duration / logRatio;
    const auto windowSize = std::min<std::size_t>(
        count, std::max<std::size_t>(256, static_cast<std::size_t>(sampleRate * 0.04)));
    for (std::size_t bin = 0; bin < analyzerBins; ++bin) {
        const auto position = static_cast<float>(bin) / static_cast<float>(analyzerBins - 1);
        const auto frequency = startHz * std::pow(endHz / startHz, position);
        const auto center = static_cast<std::size_t>(std::clamp(
            duration * std::log(static_cast<double>(frequency / startHz)) / logRatio * sampleRate,
            static_cast<double>(windowSize / 2),
            static_cast<double>(count - windowSize / 2 - 1)));
        const auto first = center - windowSize / 2;
        double exReal = 0.0, exImag = 0.0, reReal = 0.0, reImag = 0.0;
        for (std::size_t n = first; n < first + windowSize && n < count; ++n) {
            const auto t = static_cast<double>(n) / sampleRate;
            const auto phase = sweepScale * (std::pow(ratio, t / duration) - 1.0);
            const auto windowPosition = static_cast<double>(n - first) /
                static_cast<double>(std::max<std::size_t>(1, windowSize - 1));
            const auto window = 0.5 - 0.5 * std::cos(2.0 * static_cast<double>(pi) * windowPosition);
            const auto c = std::cos(phase) * window;
            const auto s = std::sin(phase) * window;
            exReal += static_cast<double>(excitation[n]) * c;
            exImag += static_cast<double>(excitation[n]) * s;
            reReal += static_cast<double>(usable[n]) * c;
            reImag += static_cast<double>(usable[n]) * s;
        }
        const auto inputMagnitude = std::hypot(exReal, exImag);
        const auto outputMagnitude = std::hypot(reReal, reImag);
        result[bin] = static_cast<float>(20.0 * std::log10((outputMagnitude + 1.0e-9) /
                                                           (inputMagnitude + 1.0e-9)));
    }
    return result;
}

inline std::array<float, analyzerBins> calibrationBiasFromResponse(
    const std::array<float, analyzerBins>& responseDb) noexcept {
    auto sorted = responseDb;
    std::sort(sorted.begin(), sorted.end());
    const auto median = sorted[sorted.size() / 2];
    std::array<float, analyzerBins> peakBias {};
    for (std::size_t i = 0; i < peakBias.size(); ++i) {
        const auto peakAboveMedian = std::max(0.0f, responseDb[i] - median - 2.0f);
        const auto centerBias = std::min(16.0f, peakAboveMedian * 1.25f);
        peakBias[i] = std::max(peakBias[i], centerBias);
        if (i > 0) peakBias[i - 1] = std::max(peakBias[i - 1], centerBias * 0.7f);
        if (i + 1 < peakBias.size()) peakBias[i + 1] = std::max(peakBias[i + 1], centerBias * 0.7f);
        if (i > 1) peakBias[i - 2] = std::max(peakBias[i - 2], centerBias * 0.35f);
        if (i + 2 < peakBias.size()) peakBias[i + 2] = std::max(peakBias[i + 2], centerBias * 0.35f);
    }
    return peakBias;
}

inline std::array<float, analyzerBins> normalizeCalibrationResponse(
    const std::array<float, analyzerBins>& responseDb) noexcept {
    constexpr float targetMedianDb = -3.0f;
    auto sorted = responseDb;
    std::sort(sorted.begin(), sorted.end());
    const auto median = 0.5f * (sorted[(sorted.size() - 1) / 2] +
                                sorted[sorted.size() / 2]);
    const auto offset = targetMedianDb - median;
    std::array<float, analyzerBins> normalized {};
    for (std::size_t bin = 0; bin < analyzerBins; ++bin)
        normalized[bin] = responseDb[bin] + offset;
    return normalized;
}

inline std::array<float, analyzerBins> detectionBaseline(
    const std::array<float, analyzerBins>& responseDb) noexcept {
    const auto peakBias = calibrationBiasFromResponse(responseDb);
    std::array<float, analyzerBins> result {};
    for (std::size_t i = 0; i < result.size(); ++i) {
        // Spread a measured resonance across adjacent logarithmic bins so a
        // live FFT peak need not land on the exact calibration bin.
        result[i] = -55.0f - peakBias[i];
    }
    return result;
}

inline float notchQuality(float frequency) noexcept {
    constexpr std::array<std::pair<float, float>, 5> reference {{
        {100.0f, 9.0f}, {500.0f, 11.0f}, {1000.0f, 14.0f},
        {4000.0f, 20.0f}, {10000.0f, 30.0f},
    }};
    if (frequency <= reference.front().first) {
        return std::max(7.0f, 9.0f - 2.0f * std::log2(
            reference.front().first / std::max(20.0f, frequency)));
    }
    for (std::size_t i = 1; i < reference.size(); ++i) {
        if (frequency <= reference[i].first) {
            const auto low = reference[i - 1];
            const auto high = reference[i];
            const auto position = std::log(frequency / low.first) /
                std::log(high.first / low.first);
            return low.second + (high.second - low.second) *
                static_cast<float>(position);
        }
    }
    return std::min(38.0f, 30.0f + 5.0f * std::log2(frequency / 10000.0f));
}

inline float notchQualityForPreset(float frequency, ProtectionPreset preset) noexcept {
    constexpr float musicQualityScale = 0.92f;
    const auto quality = notchQuality(frequency);
    return preset == ProtectionPreset::music
        ? std::max(4.0f, quality * musicQualityScale)
        : quality;
}

inline float notchQualityForCalibrationHotspot(float frequency,
                                               ProtectionPreset preset,
                                               bool calibrationHotspot) noexcept {
    const auto quality = notchQualityForPreset(frequency, preset);
    // Lower Q widens the notch. Measured sub-1 kHz room modes get a modest
    // extra width without changing ordinary live-feedback notches.
    return calibrationHotspot && frequency < 1000.0f
        ? std::max(4.0f, quality * 0.88f)
        : quality;
}

inline float probeDepthFraction(float frequency) noexcept {
    return frequency >= 300.0f && frequency <= 800.0f ? 0.75f : 0.5f;
}

inline float legacySuppressionAmount(float amount) noexcept {
    const auto clamped = std::clamp(amount, 0.0f, 1.0f);
    if (clamped <= 0.7f) return clamped;
    return std::min(1.0f, 0.7f + (clamped - 0.7f) * 3.0f);
}

inline float maximumSuppressionDepth(float amount) noexcept {
    const auto clamped = std::clamp(amount, 0.0f, 1.0f);
    const auto legacy = legacySuppressionAmount(clamped);
    const auto core = std::min(1.0f, legacy / 0.7f);
    const auto extension = std::clamp((legacy - 0.7f) / 0.3f, 0.0f, 1.0f);
    const auto extraDepth = std::clamp((clamped - 0.8f) / 0.2f, 0.0f, 1.0f) * 8.0f;
    const auto baseDepth = 14.0f * core + 10.0f * extension + extraDepth;
    const auto maximumBoost = std::clamp((clamped - 0.8f) / 0.2f, 0.0f, 1.0f) * 0.2f;
    return -baseDepth * (1.0f + maximumBoost);
}

inline float feedbackAmplitudeThresholdDb(float sensitivity) noexcept {
    const auto clamped = std::clamp(sensitivity, 0.0f, 1.0f);
    return -70.0f * clamped;
}

inline float detectorSensitivityAmount(float sensitivity,
                                       bool calibrationHotspot) noexcept {
    const auto clamped = std::clamp(sensitivity, 0.0f, 1.0f);
    return calibrationHotspot
        ? std::min(1.0f, clamped + 0.2f)
        : clamped;
}

inline float frequencyThresholdAdjustmentDb(float frequency) noexcept {
    // Keep the detector slightly conservative around low-frequency program
    // energy, neutral through the mid band, and more responsive to the
    // narrower high-frequency feedback modes.
    constexpr float lowAdjustmentDb = 5.0f;
    constexpr float highAdjustmentDb = -5.0f;
    constexpr float lowStartHz = 20.0f;
    constexpr float lowEndHz = 350.0f;
    constexpr float highStartHz = 1500.0f;
    constexpr float highEndHz = 4000.0f;
    if (frequency <= lowStartHz) return lowAdjustmentDb;
    if (frequency < lowEndHz) {
        const auto position = std::log(frequency / lowStartHz) /
            std::log(lowEndHz / lowStartHz);
        return lowAdjustmentDb * (1.0f - static_cast<float>(position));
    }
    if (frequency <= highStartHz) return 0.0f;
    if (frequency < highEndHz) {
        const auto position = std::log(frequency / highStartHz) /
            std::log(highEndHz / highStartHz);
        return highAdjustmentDb * static_cast<float>(position);
    }
    return highAdjustmentDb;
}

inline float detectorEngageThresholdDb(float sensitivity,
                                       ProtectionPreset preset,
                                       bool calibrationHotspot,
                                       float measuredPeakBias,
                                       float frequency) noexcept {
    const auto candidateSensitivity =
        detectorSensitivityAmount(sensitivity, calibrationHotspot);
    const auto candidateLegacyAmount = legacySuppressionAmount(candidateSensitivity);
    const auto candidateExtraSensitivity =
        std::clamp((candidateSensitivity - 0.8f) / 0.2f, 0.0f, 1.0f);
    const auto candidateSpeechCore =
        std::min(1.0f, candidateLegacyAmount / 0.7f);
    const auto candidateSpeechExtension =
        std::clamp((candidateLegacyAmount - 0.7f) / 0.3f, 0.0f, 1.0f);
    const auto engageAboveBaseline = preset == ProtectionPreset::speech
        ? 9.0f - 5.5f * candidateSpeechCore
            - 3.5f * candidateSpeechExtension
            - 3.0f * candidateExtraSensitivity
        : 9.0f - 1.5f * candidateLegacyAmount
            - 3.0f * candidateExtraSensitivity;
    return std::max(
        0.25f, engageAboveBaseline + frequencyThresholdAdjustmentDb(frequency) -
            (calibrationHotspot
                ? std::min(5.0f, std::max(0.0f, measuredPeakBias) * 0.65f)
                : 0.0f));
}

inline float detectorAmplitudeThresholdDb(float sensitivity,
                                          bool calibrationHotspot,
                                          float frequency) noexcept {
    return feedbackAmplitudeThresholdDb(
        detectorSensitivityAmount(sensitivity, calibrationHotspot)) +
        frequencyThresholdAdjustmentDb(frequency);
}

inline std::size_t persistentNotchLimit(std::size_t capacity, float amount) noexcept {
    const auto clamped = std::clamp(amount, 0.0f, 1.0f);
    return std::min(capacity, static_cast<std::size_t>(
        std::floor(static_cast<float>(capacity) * (2.0f / 3.0f) * clamped)));
}

inline unsigned short persistentRecurrenceWindowFrames(float latchAmount) noexcept {
    const auto clamped = std::clamp(latchAmount, 0.0f, 1.0f);
    if (clamped < 0.8f) return 420;
    const auto relaxation = std::clamp((clamped - 0.8f) / 0.2f, 0.0f, 1.0f);
    return static_cast<unsigned short>(std::lround(480.0f + 40.0f * relaxation));
}

inline int persistentRecurrenceRequirement(float latchAmount) noexcept {
    return std::clamp(latchAmount, 0.0f, 1.0f) >= 0.8f ? 3 : 6;
}

inline float persistentMinimumProbeDepthDb(float latchAmount) noexcept {
    return std::clamp(latchAmount, 0.0f, 1.0f) >= 0.8f ? 10.0f : 12.0f;
}

// Single-producer/single-consumer audio handoff. The audio callback is the
// only producer and the analysis thread is the only consumer. Full buffers
// drop the newest samples rather than overwriting samples the analyzer has not
// read yet; this keeps every FFT window internally coherent.
template <std::size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    void pushFinite(const float* data, std::size_t count) noexcept {
        const auto writePos = writeIndex.load(std::memory_order_relaxed);
        const auto readPos = readIndex.load(std::memory_order_acquire);
        const auto free = Capacity - (writePos - readPos);
        const auto toWrite = std::min(count, free);
        for (std::size_t i = 0; i < toWrite; ++i) {
            const auto value = data[i];
            buffer[(writePos + i) & mask] = std::isfinite(value) ? value : 0.0f;
        }
        writeIndex.store(writePos + toWrite, std::memory_order_release);
    }

    std::size_t pop(float* destination, std::size_t maxCount) noexcept {
        const auto writePos = writeIndex.load(std::memory_order_acquire);
        const auto readPos = readIndex.load(std::memory_order_relaxed);
        const auto available = writePos - readPos;
        const auto toRead = std::min(maxCount, available);
        for (std::size_t i = 0; i < toRead; ++i)
            destination[i] = buffer[(readPos + i) & mask];
        readIndex.store(readPos + toRead, std::memory_order_release);
        return toRead;
    }

private:
    static constexpr std::size_t mask = Capacity - 1;
    std::array<float, Capacity> buffer {};
    std::atomic<std::size_t> writeIndex { 0 }, readIndex { 0 };
};

class FeedbackProcessor {
public:
    void prepare(double newSampleRate) noexcept {
        sampleRate = std::max(8000.0, newSampleRate);
        reset();
        clearBaseline();
    }
    void clearBaseline() noexcept {
        for (std::size_t i = 0; i < analyzerBins; ++i) {
            baseline[i].store(-55.0f, std::memory_order_relaxed);
            calibrationPeakBias[i].store(0.0f, std::memory_order_relaxed);
        }
    }
    void reset() noexcept {
        detectorSamples = 0;
        analysisBuffer.fill(0.0f);
        persistence.fill(0);
        growthFrames.fill(0);
        previousLevel.fill(-120.0f);
        probeCooldown.fill(0);
        notchSeen.fill(false);
        analysisNotches = {};
        states = {};
        spectrumDb.fill(-120.0f);
        for (std::size_t i = 0; i < analyzerBins; ++i) publishedSpectrum[i].store(-120.0f);
        for (std::size_t i = 0; i < maxNotches; ++i) {
            publishedFrequency[i].store(0.0f);
            publishedDepth[i].store(0.0f);
            publishedQ[i].store(0.0f);
            targetFrequency[i].store(0.0f);
            targetDepth[i].store(0.0f);
            targetQ[i].store(0.0f);
            targetSinW[i].store(0.0f);
            targetNegTwoCosW[i].store(0.0f);
            targetHotspot[i].store(false);
            targetSequence[i].store(0, std::memory_order_relaxed);
        }
        float discard[1024];
        while (inputRing->pop(discard, 1024) > 0) {}
    }
    void setEnabled(bool value) noexcept { enabled.store(value, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }
    void setNotchCapacity(std::size_t value) noexcept {
        notchCapacity.store(std::min(maxNotches, value), std::memory_order_release);
    }
    std::size_t getNotchCapacity() const noexcept {
        return notchCapacity.load(std::memory_order_acquire);
    }
    void requestNotchReset() noexcept {
        notchResetGeneration.fetch_add(1, std::memory_order_release);
    }
    void setPreset(ProtectionPreset value) noexcept { preset.store(value, std::memory_order_relaxed); }
    void setSuppressionAmount(float value) noexcept {
        setDepthAmount(value);
    }
    float getSuppressionAmount() const noexcept {
        return getDepthAmount();
    }
    void setDepthAmount(float value) noexcept {
        depthAmount.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float getDepthAmount() const noexcept {
        return depthAmount.load(std::memory_order_relaxed);
    }
    void setSensitivityAmount(float value) noexcept {
        sensitivityAmount.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float getSensitivityAmount() const noexcept {
        return sensitivityAmount.load(std::memory_order_relaxed);
    }
    void setTimingAmount(float value) noexcept {
        timingAmount.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float getTimingAmount() const noexcept {
        return timingAmount.load(std::memory_order_relaxed);
    }
    void setLatchAmount(float value) noexcept {
        latchAmount.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float getLatchAmount() const noexcept {
        return latchAmount.load(std::memory_order_relaxed);
    }
    void setManualNotch(float frequency) noexcept {
        if (!std::isfinite(frequency) || frequency < 40.0f || frequency > 20000.0f) return;
        for (std::size_t i = 0; i < maxNotches; ++i) {
            const auto current = manualNotchFrequency[i].load(std::memory_order_relaxed);
            if (current > 0.0f && std::abs(std::log2(current / frequency)) < 0.08f) {
                manualNotchFrequency[i].store(frequency, std::memory_order_release);
                return;
            }
        }
        for (std::size_t i = 0; i < maxNotches; ++i) {
            float empty = 0.0f;
            if (manualNotchFrequency[i].compare_exchange_strong(
                    empty, frequency, std::memory_order_release, std::memory_order_relaxed))
                return;
        }
    }
    void clearManualNotch(float frequency) noexcept {
        if (!std::isfinite(frequency) || frequency <= 0.0f) return;
        for (std::size_t i = 0; i < maxNotches; ++i) {
            const auto current = manualNotchFrequency[i].load(std::memory_order_relaxed);
            if (current > 0.0f && std::abs(std::log2(current / frequency)) < 0.08f)
                manualNotchFrequency[i].store(0.0f, std::memory_order_release);
        }
    }
    void setBaseline(const std::array<float, analyzerBins>& value) noexcept {
        for (std::size_t i = 0; i < analyzerBins; ++i) {
            baseline[i].store(value[i], std::memory_order_relaxed);
            calibrationPeakBias[i].store(0.0f, std::memory_order_relaxed);
        }
    }
    void setCalibrationProfile(const std::array<float, analyzerBins>& responseDb) noexcept {
        const auto calibratedBaseline = detectionBaseline(responseDb);
        const auto calibratedPeakBias = calibrationBiasFromResponse(responseDb);
        for (std::size_t i = 0; i < analyzerBins; ++i) {
            baseline[i].store(calibratedBaseline[i], std::memory_order_relaxed);
            calibrationPeakBias[i].store(calibratedPeakBias[i], std::memory_order_relaxed);
        }
    }

    float process(float sample) noexcept {
        if (!std::isfinite(sample)) return 0.0f;
        auto wet = sample;
        const auto capacity = getNotchCapacity();
        for (std::size_t i = 0; i < capacity; ++i)
            wet = states[i].process(wet);
        if (!std::isfinite(wet)) wet = 0.0f;
        const auto targetMix = isEnabled() ? 1.0f : 0.0f;
        wetMix += (targetMix - wetMix) * 0.0015f;
        const auto output = sample + wetMix * (wet - sample);
        return std::isfinite(output) ? output : 0.0f;
    }

    // Called once at the start of each audio block. The analysis thread
    // publishes complete target snapshots; the sequence check prevents the
    // callback from combining a new frequency with an old depth or Q.
    void pullPendingNotchUpdates() noexcept {
        const auto requestedReset = notchResetGeneration.load(std::memory_order_acquire);
        if (requestedReset != appliedNotchResetGeneration) {
            for (auto& state : states) {
                state = {};
                state.sampleRate = sampleRate;
            }
            appliedNotchResetGeneration = requestedReset;
            publishedNotchResetGeneration.store(requestedReset, std::memory_order_release);
        }
        const auto capacity = getNotchCapacity();
        const auto releaseSmoothing = 0.00035f + 0.0009f * (1.0f - getTimingAmount());
        for (std::size_t i = 0; i < capacity; ++i) {
            const auto before = targetSequence[i].load(std::memory_order_acquire);
            if ((before & 1u) != 0u) continue;
            const auto frequency = targetFrequency[i].load(std::memory_order_relaxed);
            const auto depth = targetDepth[i].load(std::memory_order_relaxed);
            const auto q = targetQ[i].load(std::memory_order_relaxed);
            const auto sinW = targetSinW[i].load(std::memory_order_relaxed);
            const auto negTwoCosW = targetNegTwoCosW[i].load(std::memory_order_relaxed);
            const auto hotspot = targetHotspot[i].load(std::memory_order_relaxed);
            const auto after = targetSequence[i].load(std::memory_order_acquire);
            if (before != after || (after & 1u) != 0u) continue;

            auto& state = states[i];
            state.releaseSmoothing = releaseSmoothing;
            if (frequency <= 0.0f && state.frequency > 0.0f) {
                state = {};
                state.sampleRate = sampleRate;
            } else if (frequency > 0.0f) {
                state.frequency = frequency;
                state.targetDepthDb = depth;
                state.q = q;
                state.sinW = sinW;
                state.negTwoCosW = negTwoCosW;
                state.calibrationHotspot = hotspot;
            }
        }
        for (std::size_t i = 0; i < capacity; ++i) {
            publishedFrequency[i].store(states[i].frequency, std::memory_order_relaxed);
            publishedDepth[i].store(states[i].currentDepthDb, std::memory_order_relaxed);
            publishedQ[i].store(states[i].q, std::memory_order_relaxed);
        }
    }

    // Copy only the raw input into the SPSC handoff. This is linear work with
    // no FFT, lock, allocation, or unbounded retry on the audio thread.
    void pushAnalysisBlock(const float* block, int numSamples) noexcept {
        if (block == nullptr || numSamples <= 0) return;
        inputRing->pushFinite(block, static_cast<std::size_t>(numSamples));
    }

    // Called only by the dedicated analysis thread.
    bool pumpBackgroundAnalysis() noexcept {
        const auto requestedReset = notchResetGeneration.load(std::memory_order_acquire);
        if (requestedReset != analyzedNotchResetGeneration) {
            persistence.fill(0);
            repeatHits.fill(0);
            probeCooldown.fill(0);
            repeatHitAge.fill(0);
            growthFrames.fill(0);
            previousLevel.fill(-120.0f);
            notchSeen.fill(false);
            analysisNotches = {};
            for (std::size_t i = 0; i < maxNotches; ++i) {
                targetFrequency[i].store(0.0f, std::memory_order_relaxed);
                targetDepth[i].store(0.0f, std::memory_order_relaxed);
                targetQ[i].store(0.0f, std::memory_order_relaxed);
                targetHotspot[i].store(false, std::memory_order_relaxed);
            }
            analyzedNotchResetGeneration = requestedReset;
        }
        float scratch[1024];
        const auto popped = inputRing->pop(scratch, 1024);
        for (std::size_t i = 0; i < popped; ++i) analyzeSample(scratch[i]);
        return popped > 0;
    }

    ProtectionSnapshot snapshot() const noexcept {
        ProtectionSnapshot result;
        for (std::size_t i = 0; i < analyzerBins; ++i)
            result.spectrumDb[i] = publishedSpectrum[i].load(std::memory_order_relaxed);
        const auto capacity = getNotchCapacity();
        const auto resetPending =
            publishedNotchResetGeneration.load(std::memory_order_acquire)
            != notchResetGeneration.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < capacity && !resetPending; ++i) {
            const auto frequency = publishedFrequency[i].load(std::memory_order_relaxed);
            const auto depth = publishedDepth[i].load(std::memory_order_relaxed);
            const auto q = publishedQ[i].load(std::memory_order_relaxed);
            // Do not report a notch while it is only carrying a sub-dB
            // release tail; the UI and telemetry should describe audible
            // protection moves, not filter state that is already gone.
            result.notches[i] = { frequency, depth, q, depth < -1.0f };
            if (result.notches[i].active) {
                ++result.activeNotches;
                result.maximumCutDb = std::min(result.maximumCutDb, depth);
            }
        }
        result.depthAmount = getDepthAmount();
        result.sensitivityAmount = getSensitivityAmount();
        result.suppressionAmount = result.depthAmount;
        result.timingAmount = getTimingAmount();
        result.latchAmount = getLatchAmount();
        return result;
    }

private:
    struct Notch {
        float frequency = 0, q = 8, currentDepthDb = 0, targetDepthDb = 0;
        bool calibrationHotspot = false;
        int releaseHoldFrames = 8;
        int quietFrames = 0;
        float releaseSmoothing = 0.00025f;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float sinW = 0, negTwoCosW = 0;
        int coefficientCountdown = 0;
        double sampleRate = 48000;
        float process(float x) noexcept {
            const auto movingDeeper = targetDepthDb < currentDepthDb;
            const auto smoothing = movingDeeper ? 0.0014f : releaseSmoothing;
            currentDepthDb += (targetDepthDb - currentDepthDb) * smoothing;
            if (std::abs(currentDepthDb) < 0.005f || frequency <= 0) return x;
            if (coefficientCountdown-- <= 0) {
                coefficientCountdown = 31;
                const auto alpha = sinW / (2.0f * q);
                const auto gain = std::pow(10.0f, currentDepthDb / 40.0f);
                const auto denominator = 1.0f + alpha / gain;
                b0 = (1.0f + alpha * gain) / denominator;
                b1 = negTwoCosW / denominator;
                b2 = (1.0f - alpha * gain) / denominator;
                a1 = negTwoCosW / denominator;
                a2 = (1.0f - alpha / gain) / denominator;
            }
            const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            return std::isfinite(y) ? y : 0.0f;
        }
    };

    struct ShadowNotch {
        float frequency = 0, q = 8, targetDepthDb = 0;
        bool calibrationHotspot = false;
        bool persistent = false;
        bool manual = false;
        bool awaitingConfirmation = false;
        bool probeWasRecurrence = false;
        int probeFrames = 0;
        float probeReferenceLevelDb = -120.0f;
        float probeDepthDb = 0.0f;
        int releaseHoldFrames = 8;
        int quietFrames = 0;
    };

    void syncManualNotches(std::size_t capacity) noexcept {
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& notch = analysisNotches[i];
            if (!notch.manual) continue;
            bool stillRequested = false;
            for (const auto& requested : manualNotchFrequency) {
                const auto frequency = requested.load(std::memory_order_acquire);
                if (frequency > 0.0f &&
                    std::abs(std::log2(frequency / notch.frequency)) < 0.08f) {
                    stillRequested = true;
                    break;
                }
            }
            if (!stillRequested) notch = {};
        }

        for (const auto& requested : manualNotchFrequency) {
            const auto frequency = requested.load(std::memory_order_acquire);
            if (frequency <= 0.0f) continue;
            ShadowNotch* selected = nullptr;
            for (std::size_t i = 0; i < capacity; ++i) {
                auto& notch = analysisNotches[i];
                if (notch.manual && std::abs(std::log2(notch.frequency / frequency)) < 0.08f) {
                    selected = &notch;
                    break;
                }
            }
            if (selected == nullptr) {
                for (std::size_t i = 0; i < capacity; ++i) {
                    auto& notch = analysisNotches[i];
                    if (!notch.manual && !notch.persistent &&
                        (notch.frequency <= 0.0f || notch.targetDepthDb >= -0.1f)) {
                        selected = &notch;
                        break;
                    }
                }
            }
            if (selected == nullptr) continue;
            selected->frequency = frequency;
            selected->q = notchQuality(frequency);
            selected->targetDepthDb = maximumSuppressionDepth(getDepthAmount());
            selected->calibrationHotspot = false;
            selected->persistent = true;
            selected->manual = true;
            selected->releaseHoldFrames = std::numeric_limits<int>::max();
            selected->quietFrames = 0;
            const auto index = static_cast<std::size_t>(
                selected - analysisNotches.data());
            notchSeen[index] = true;
        }
    }

    void analyzeSample(float sample) noexcept {
        const auto capacity = getNotchCapacity();
        analysisBuffer[static_cast<std::size_t>(detectorSamples)] = sample;
        if (++detectorSamples < static_cast<int>(fftSize)) return;
        for (std::size_t i = 0; i < fftSize; ++i) {
            const auto window = 0.5f - 0.5f * std::cos(
                2.0f * pi * static_cast<float>(i) / static_cast<float>(fftSize - 1));
            fftReal[i] = analysisBuffer[i] * window;
            fftImag[i] = 0.0f;
        }
        for (std::size_t i = 1, j = 0; i < fftSize; ++i) {
            auto bit = fftSize >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(fftReal[i], fftReal[j]); std::swap(fftImag[i], fftImag[j]); }
        }
        for (std::size_t length = 2; length <= fftSize; length <<= 1) {
            const auto angle = -2.0f * pi / static_cast<float>(length);
            const auto stepReal = std::cos(angle), stepImag = std::sin(angle);
            for (std::size_t start = 0; start < fftSize; start += length) {
                float wReal = 1.0f, wImag = 0.0f;
                for (std::size_t offset = 0; offset < length / 2; ++offset) {
                    const auto even = start + offset, odd = even + length / 2;
                    const auto oddReal = fftReal[odd] * wReal - fftImag[odd] * wImag;
                    const auto oddImag = fftReal[odd] * wImag + fftImag[odd] * wReal;
                    fftReal[odd] = fftReal[even] - oddReal; fftImag[odd] = fftImag[even] - oddImag;
                    fftReal[even] += oddReal; fftImag[even] += oddImag;
                    const auto nextReal = wReal * stepReal - wImag * stepImag;
                    wImag = wReal * stepImag + wImag * stepReal; wReal = nextReal;
                }
            }
        }
        const auto selectedPreset = preset.load(std::memory_order_relaxed);
        const auto amount = getSensitivityAmount();
        notchSeen.fill(false);
        syncManualNotches(capacity);
        for (std::size_t bin = 0; bin < repeatHits.size(); ++bin) {
            if (repeatHits[bin] == 0) continue;
            repeatHitAge[bin] = static_cast<unsigned short>(
                std::min<unsigned int>(65535u, repeatHitAge[bin] + 1u));
            if (repeatHitAge[bin] > persistentRecurrenceWindowFrames(getLatchAmount())) {
                repeatHits[bin] = 0;
                repeatHitAge[bin] = 0;
            }
        }
        for (std::size_t bin = 0; bin < analyzerBins; ++bin) {
            const auto position = static_cast<float>(bin) / static_cast<float>(analyzerBins - 1);
            const auto frequency = 20.0f * std::pow(1000.0f, position);
            const auto fftBin = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::lround(frequency * fftSize / sampleRate)), 1, fftSize / 2 - 1);
            const auto magnitude = 4.0f * std::hypot(fftReal[fftBin], fftImag[fftBin]) /
                                   static_cast<float>(fftSize);
            spectrumDb[bin] = 20.0f * std::log10(magnitude + 1.0e-9f);
        }
        updatePendingProbes(capacity);
        detectorSamples = 0;
        std::array<std::size_t, maxNotches> candidateBins {};
        std::array<float, maxNotches> candidateScores {};
        std::array<float, maxNotches> candidateLevels {};
        candidateScores.fill(-std::numeric_limits<float>::infinity());
        // The overlapping hop makes three speech frames about 16 ms apart while
        // still requiring a stable, rising tonal peak rather than a single
        // voice or music bin.
        const auto firstBin = std::max<std::size_t>(2, static_cast<std::size_t>(40.0 * fftSize / sampleRate));
        const auto lastBin = std::min<std::size_t>(fftSize / 2 - 2,
            static_cast<std::size_t>(std::min(20000.0, sampleRate * 0.45) * fftSize / sampleRate));
        for (std::size_t fftBin = firstBin; fftBin <= lastBin; ++fftBin) {
            if (probeCooldown[fftBin] > 0) {
                --probeCooldown[fftBin];
                continue;
            }
            const auto frequency = static_cast<float>(fftBin * sampleRate / fftSize);
            const auto logPosition = std::log10(frequency / 20.0f) / std::log10(1000.0f);
            const auto baselineBin = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::lround(logPosition * (analyzerBins - 1))), 0, analyzerBins - 1);
            const auto magnitude = 4.0f * std::hypot(fftReal[fftBin], fftImag[fftBin]) /
                                   static_cast<float>(fftSize);
            const auto level = 20.0f * std::log10(magnitude + 1.0e-9f);
            const auto levelDelta = level - previousLevel[fftBin];
            if (levelDelta > (selectedPreset == ProtectionPreset::speech ? 0.2f : 0.35f))
                growthFrames[fftBin] = static_cast<unsigned char>(
                    std::min<int>(255, growthFrames[fftBin] + 1));
            else if (growthFrames[fftBin] > 0)
                --growthFrames[fftBin];
            previousLevel[fftBin] = level;
            const auto neighborhoodRadius = std::min<std::size_t>(
                { 8, fftBin - 1, fftSize / 2 - fftBin - 1 });
            double neighborhoodPower = 0.0;
            std::size_t neighborhoodBins = 0;
            for (std::size_t offset = 2; offset <= neighborhoodRadius; ++offset) {
                neighborhoodPower += std::hypot(fftReal[fftBin - offset], fftImag[fftBin - offset]);
                neighborhoodPower += std::hypot(fftReal[fftBin + offset], fftImag[fftBin + offset]);
                neighborhoodBins += 2;
            }
            const auto neighborhoodLevel = 20.0f * std::log10(
                4.0f * static_cast<float>(neighborhoodPower /
                    static_cast<double>(std::max<std::size_t>(1, neighborhoodBins))) /
                static_cast<float>(fftSize) + 1.0e-9f);
            const auto excess = level - baseline[baselineBin].load(std::memory_order_relaxed);
            const auto measuredPeakBias =
                calibrationPeakBias[baselineBin].load(std::memory_order_relaxed);
            const auto calibrationHotspot = measuredPeakBias >= 1.5f;
            // A calibrated hotspot is known to be unsafe, so give it a
            // sensitivity lift above the route slider before applying the
            // ordinary baseline-relative detector gates.
            const auto candidateSensitivity =
                detectorSensitivityAmount(amount, calibrationHotspot);
            const auto candidateExtraSensitivity =
                std::clamp((candidateSensitivity - 0.8f) / 0.2f, 0.0f, 1.0f);
            const auto calibratedEngageThreshold = detectorEngageThresholdDb(
                amount, selectedPreset, calibrationHotspot, measuredPeakBias, frequency);
            const auto amplitudeThresholdDb = detectorAmplitudeThresholdDb(
                amount, calibrationHotspot, frequency);
            // Broad speech fundamentals and harmonics are less likely to pass
            // this wider neighborhood comparison than a narrow room howl.
            const auto tonal = level - neighborhoodLevel;
            const auto tonalThreshold = selectedPreset == ProtectionPreset::music
                ? (frequency > 1000.0f ? 6.0f - 0.9f * candidateExtraSensitivity
                    : 7.0f - 0.8f * candidateExtraSensitivity)
                : (frequency < 350.0f ? 4.5f : (frequency > 1000.0f ? 2.0f : 2.5f));
            const auto leftMagnitude = 4.0f * std::hypot(fftReal[fftBin - 1], fftImag[fftBin - 1]) /
                                       static_cast<float>(fftSize);
            const auto rightMagnitude = 4.0f * std::hypot(fftReal[fftBin + 1], fftImag[fftBin + 1]) /
                                        static_cast<float>(fftSize);
            const auto localPeak = magnitude >= leftMagnitude && magnitude >= rightMagnitude;
            const auto risingPeak = growthFrames[fftBin] >=
                (selectedPreset == ProtectionPreset::speech ? 1 : 2);
            // Once a narrow peak clears the preset's lower baseline gate,
            // persistence is enough to distinguish sustained feedback from a
            // transient rise. The former 24 dB floor blocked quieter howls
            // even after the baseline threshold had been lowered.
            const auto stableStrongPeak = selectedPreset == ProtectionPreset::speech
                ? excess >= calibratedEngageThreshold
                : excess >= std::max(15.0f, 24.0f - 6.0f * candidateExtraSensitivity);
            if (localPeak && level >= amplitudeThresholdDb &&
                excess >= calibratedEngageThreshold && tonal >= tonalThreshold &&
                (risingPeak || stableStrongPeak)) {
                persistence[fftBin] = static_cast<unsigned char>(
                    std::min<int>(255, persistence[fftBin] + 1));
                const auto requiredFrames = selectedPreset == ProtectionPreset::speech
                    // Music waits for a longer, more convincing narrow peak
                    // before even starting the shallow confirmation cut.
                    // Speech keeps the existing fastest possible response.
                    ? 1 : (frequency > 1000.0f ? 32 : 48);
                if (persistence[fftBin] < requiredFrames) continue;
                const auto score = excess + tonal;
                for (std::size_t slot = 0; slot < capacity; ++slot) {
                    if (score <= candidateScores[slot]) continue;
                    for (std::size_t move = capacity - 1; move > slot; --move) {
                        candidateScores[move] = candidateScores[move - 1];
                        candidateBins[move] = candidateBins[move - 1];
                        candidateLevels[move] = candidateLevels[move - 1];
                    }
                    candidateScores[slot] = score;
                    candidateBins[slot] = fftBin;
                    candidateLevels[slot] = level;
                    break;
                }
            } else {
                persistence[fftBin] = static_cast<unsigned char>(
                    persistence[fftBin] > 1 ? persistence[fftBin] - 2 : 0);
            }
        }
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& notch = analysisNotches[i];
            if (!notch.persistent || notch.manual) continue;
            if (getLatchAmount() <= 0.001f && !notchSeen[i]) {
                notch.persistent = false;
                notch.targetDepthDb = 0.0f;
                notch.quietFrames = notch.releaseHoldFrames + 1;
                continue;
            }
            notchSeen[i] = true;
            notch.targetDepthDb = std::min(
                notch.targetDepthDb, maximumSuppressionDepth(amount));
        }
        std::array<std::size_t, maxNotches> engagedBins {};
        std::size_t engagedCount = 0;
        for (std::size_t candidateIndex = 0; candidateIndex < capacity; ++candidateIndex) {
            const auto candidate = candidateBins[candidateIndex];
            if (candidate == 0) continue;
            const auto distinct = std::none_of(engagedBins.begin(), engagedBins.begin() + engagedCount,
                [candidate](std::size_t other) { return std::abs(static_cast<int>(other) - static_cast<int>(candidate)) < 2; });
            if (distinct) {
                engagedBins[engagedCount++] = candidate;
                const auto candidateFrequency =
                    static_cast<float>(candidate * sampleRate / fftSize);
                const auto candidateLogPosition =
                    std::log10(candidateFrequency / 20.0f) / std::log10(1000.0f);
                const auto candidateBaselineBin = std::clamp<std::size_t>(
                    static_cast<std::size_t>(std::lround(
                        candidateLogPosition * (analyzerBins - 1))),
                    0, analyzerBins - 1);
                const auto calibrationHotspot =
                    calibrationPeakBias[candidateBaselineBin].load(
                        std::memory_order_relaxed) >= 1.5f;
                engageFrequency(candidateFrequency, calibrationHotspot,
                                candidateLevels[candidateIndex]);
            }
        }
        consolidateCoupledNotches(capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& notch = analysisNotches[i];
            if (notch.frequency <= 0 || notchSeen[i]) continue;
            // Release from candidate absence as well as level. This prevents a
            // stale low-frequency notch from re-engaging itself after the room
            // has gone quiet.
            ++notch.quietFrames;
            const auto amount = getDepthAmount();
            const auto timing = getTimingAmount();
            const auto latch = getLatchAmount();
            const auto releaseHoldScale = 0.5f + 0.5f * latch;
            const auto effectiveReleaseHoldFrames = static_cast<int>(std::ceil(
                static_cast<float>(notch.releaseHoldFrames) * releaseHoldScale));
            if (notch.quietFrames <= effectiveReleaseHoldFrames) continue;
            const auto ordinaryReleaseStep = 0.15f - 0.08f * amount;
            const auto hotspotReleaseFactor = 0.7f - 0.35f * timing +
                (1.0f - latch) * (0.4f + 0.3f * timing);
            const auto releaseStep = notch.calibrationHotspot
                ? ordinaryReleaseStep * hotspotReleaseFactor *
                    (1.0f - 0.45f * latch)
                : ordinaryReleaseStep * (1.0f - 0.7f * timing) *
                    (1.0f - 0.45f * latch);
            notch.targetDepthDb = std::min(0.0f, notch.targetDepthDb + releaseStep);
            if (notch.targetDepthDb >= -0.1f &&
                publishedDepth[i].load(std::memory_order_relaxed) > -0.5f)
                notch = {};
        }
        for (std::size_t i = 0; i < analyzerBins; ++i)
            publishedSpectrum[i].store(spectrumDb[i], std::memory_order_relaxed);
        for (std::size_t i = 0; i < capacity; ++i) {
            const auto sequence = targetSequence[i].load(std::memory_order_relaxed);
            targetSequence[i].store(sequence + 1u, std::memory_order_release);
            targetFrequency[i].store(analysisNotches[i].frequency, std::memory_order_relaxed);
            targetDepth[i].store(analysisNotches[i].targetDepthDb, std::memory_order_relaxed);
            targetQ[i].store(analysisNotches[i].q, std::memory_order_relaxed);
            const auto frequency = analysisNotches[i].frequency;
            const auto q = analysisNotches[i].q;
            const auto w = frequency > 0.0f && q > 0.0f
                ? 2.0f * pi * frequency / static_cast<float>(sampleRate)
                : 0.0f;
            targetSinW[i].store(w == 0.0f ? 0.0f : std::sin(w), std::memory_order_relaxed);
            targetNegTwoCosW[i].store(w == 0.0f ? 0.0f : -2.0f * std::cos(w),
                                      std::memory_order_relaxed);
            targetHotspot[i].store(analysisNotches[i].calibrationHotspot, std::memory_order_relaxed);
            targetSequence[i].store(sequence + 2u, std::memory_order_release);
        }
        std::copy(analysisBuffer.begin() + static_cast<std::ptrdiff_t>(analysisHop),
                  analysisBuffer.end(), analysisBuffer.begin());
        detectorSamples = fftSize - analysisHop;
    }
    float peakLevelNear(float frequency, float& peakFrequency) const noexcept {
        const auto centerBin = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::lround(frequency * fftSize / sampleRate)),
            1, fftSize / 2 - 1);
        const auto first = centerBin > 3 ? centerBin - 3 : 1;
        const auto last = std::min<std::size_t>(fftSize / 2 - 1, centerBin + 3);
        auto bestBin = centerBin;
        float bestMagnitude = 0.0f;
        for (auto bin = first; bin <= last; ++bin) {
            const auto magnitude = std::hypot(fftReal[bin], fftImag[bin]);
            if (magnitude > bestMagnitude) {
                bestMagnitude = magnitude;
                bestBin = bin;
            }
        }
        peakFrequency = static_cast<float>(bestBin * sampleRate / fftSize);
        return 20.0f * std::log10(
            4.0f * bestMagnitude / static_cast<float>(fftSize) + 1.0e-9f);
    }
    void updatePendingProbes(std::size_t capacity) noexcept {
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& notch = analysisNotches[i];
            if (!notch.awaitingConfirmation || notch.frequency <= 0.0f) continue;
            notchSeen[i] = true;
            ++notch.probeFrames;
            // The probe must remain in the signal for several overlapping
            // windows before judging its response. This keeps the decision
            // independent of the analysis/render thread handoff timing.
            if (notch.probeFrames < probeConfirmationFrames) continue;

            float measuredFrequency = notch.frequency;
            const auto measuredLevel = peakLevelNear(notch.frequency, measuredFrequency);
            const auto attenuation = notch.probeReferenceLevelDb - measuredLevel;
            const auto expectedAttenuation = std::abs(notch.probeDepthDb);
            const auto pitchStable =
                std::abs(std::log2(std::max(40.0f, measuredFrequency) /
                                   std::max(40.0f, notch.frequency))) < 0.025f;
            // A real feedback loop keeps the same narrow peak while the
            // probe removes roughly its own amount of level. A normal
            // program tone is rejected when the peak moves or collapses
            // substantially beyond the probe's expected attenuation.
            const auto expectedResponse = attenuation >=
                std::max(1.5f, expectedAttenuation * 0.45f) &&
                attenuation <= expectedAttenuation + 4.5f;
            if (!pitchStable || !expectedResponse) {
                probeCooldown[std::clamp<std::size_t>(
                    static_cast<std::size_t>(std::lround(
                        notch.frequency * fftSize / sampleRate)),
                    1, fftSize / 2 - 1)] =
                    persistentRecurrenceWindowFrames(getLatchAmount());
                notch.awaitingConfirmation = false;
                notch.targetDepthDb = 0.0f;
                notch.quietFrames = notch.releaseHoldFrames + 1;
                continue;
            }

            notch.awaitingConfirmation = false;
            const auto centerBin = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::lround(
                    measuredFrequency * fftSize / sampleRate)),
                1, fftSize / 2 - 1);
            if (notch.probeWasRecurrence) {
                repeatHits[centerBin] = static_cast<unsigned char>(
                    std::min<int>(255, repeatHits[centerBin] + 1));
                repeatHitAge[centerBin] = 0;
            }
            const auto persistentLimit = persistentNotchLimit(
                capacity, getLatchAmount());
            const auto deepEnoughForHold = std::abs(notch.probeDepthDb) >=
                persistentMinimumProbeDepthDb(getLatchAmount());
            std::size_t persistentCount = 0;
            for (std::size_t index = 0; index < capacity; ++index)
                if (analysisNotches[index].persistent &&
                    !analysisNotches[index].manual)
                    ++persistentCount;
            if (!notch.manual && !notch.persistent &&
                notch.calibrationHotspot &&
                persistentCount < persistentLimit &&
                deepEnoughForHold &&
                repeatHits[centerBin] >= persistentRecurrenceRequirement(getLatchAmount()))
                notch.persistent = true;
            notch.targetDepthDb = maximumSuppressionDepth(getDepthAmount());
        }
    }
    void consolidateCoupledNotches(std::size_t capacity) noexcept {
        // A low-frequency room mode can drift across a few nearby FFT peaks.
        // When expanded route capacity lets adjacent cuts collect in one
        // half-octave area, use one wider and slightly deeper cut instead.
        constexpr float maximumClusterFrequency = 2000.0f;
        constexpr float maximumClusterSpanOctaves = 1.0f / 2.0f;
        std::array<std::size_t, maxNotches> sortedIndices {};
        std::size_t count = 0;
        for (std::size_t i = 0; i < capacity; ++i) {
            const auto& notch = analysisNotches[i];
            if (notch.frequency <= 0.0f || notch.frequency > maximumClusterFrequency ||
                notch.targetDepthDb >= -0.1f || notch.manual)
                continue;
            sortedIndices[count++] = i;
        }
        std::sort(sortedIndices.begin(), sortedIndices.begin() +
            static_cast<std::ptrdiff_t>(count), [this](std::size_t left, std::size_t right) {
                return analysisNotches[left].frequency < analysisNotches[right].frequency;
            });

        std::size_t groupStart = 0;
        while (groupStart < count) {
            std::size_t groupEnd = groupStart + 1;
            const auto groupFloor = analysisNotches[sortedIndices[groupStart]].frequency;
            while (groupEnd < count) {
                const auto frequency = analysisNotches[sortedIndices[groupEnd]].frequency;
                if (std::log2(frequency / groupFloor) > maximumClusterSpanOctaves) break;
                ++groupEnd;
            }
            const auto groupSize = groupEnd - groupStart;
            if (groupSize < 2) {
                ++groupStart;
                continue;
            }

            std::size_t survivor = sortedIndices[groupStart];
            float weightedLogFrequency = 0.0f;
            float totalWeight = 0.0f;
            float deepestDepth = 0.0f;
            bool calibrationHotspot = false;
            bool persistent = false;
            int releaseHoldFrames = 0;
            for (std::size_t position = groupStart; position < groupEnd; ++position) {
                const auto index = sortedIndices[position];
                const auto& notch = analysisNotches[index];
                const auto weight = std::max(1.0f, -notch.targetDepthDb);
                weightedLogFrequency += weight * std::log(notch.frequency);
                totalWeight += weight;
                if (notch.targetDepthDb < deepestDepth) {
                    deepestDepth = notch.targetDepthDb;
                    survivor = index;
                }
                calibrationHotspot = calibrationHotspot || notch.calibrationHotspot;
                persistent = persistent || notch.persistent;
                releaseHoldFrames = std::max(releaseHoldFrames, notch.releaseHoldFrames);
            }
            const auto centerFrequency = std::exp(weightedLogFrequency /
                std::max(1.0f, totalWeight));
            const auto maximumDepth = maximumSuppressionDepth(getDepthAmount());
            const auto addedDepth = std::min(3.0f,
                1.0f + 0.75f * static_cast<float>(groupSize - 2));
            auto& merged = analysisNotches[survivor];
            merged.frequency = centerFrequency;
            merged.q = std::max(4.0f, notchQualityForCalibrationHotspot(
                centerFrequency, preset.load(std::memory_order_relaxed),
                calibrationHotspot) / (1.0f + 0.25f * static_cast<float>(groupSize - 1)));
            merged.targetDepthDb = std::max(maximumDepth, deepestDepth - addedDepth);
            merged.calibrationHotspot = calibrationHotspot;
            merged.persistent = persistent;
            merged.releaseHoldFrames = releaseHoldFrames;
            merged.quietFrames = 0;
            notchSeen[survivor] = true;
            for (std::size_t position = groupStart; position < groupEnd; ++position) {
                const auto index = sortedIndices[position];
                if (index == survivor) continue;
                analysisNotches[index] = {};
                notchSeen[index] = false;
            }
            groupStart = groupEnd;
        }
    }
    void engageFrequency(float frequency, bool calibrationHotspot,
                         float levelDb) noexcept {
        const auto capacity = getNotchCapacity();
        if (capacity == 0) return;
        const auto candidateBin = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::lround(frequency * fftSize / sampleRate)),
            1, fftSize / 2 - 1);
        if (probeCooldown[candidateBin] > 0) return;
        ShadowNotch* selected = nullptr;
        for (std::size_t i = 0; i < capacity; ++i) {
            auto& notch = analysisNotches[i];
            if (notch.frequency > 0 && std::abs(std::log2(notch.frequency / frequency)) < 0.08f) {
                selected = &notch;
                break;
            }
        }
        if (!selected) {
            for (std::size_t i = 0; i < capacity; ++i) {
                const auto& notch = analysisNotches[i];
                if ((notch.frequency <= 0 || (notch.targetDepthDb >= -0.1f &&
                    publishedDepth[i].load(std::memory_order_relaxed) > -0.5f))
                    && !notch.persistent && !notch.manual) {
                    selected = &analysisNotches[i];
                    break;
                }
            }
        }
        if (!selected) {
            std::size_t weakest = 0;
            bool foundWeakest = !analysisNotches[0].persistent && !analysisNotches[0].manual;
            for (std::size_t i = 1; i < capacity; ++i) {
                if (!analysisNotches[i].persistent && !analysisNotches[i].manual &&
                    (!foundWeakest ||
                    publishedDepth[i].load(std::memory_order_relaxed) >
                    publishedDepth[weakest].load(std::memory_order_relaxed))
                    ) {
                    weakest = i;
                    foundWeakest = true;
                }
            }
            if (!foundWeakest) return;
            selected = &analysisNotches[weakest];
        }
        const auto selectedIndex = static_cast<std::size_t>(selected - analysisNotches.data());
        const auto quietFramesBeforeEngagement = selected->quietFrames;
        notchSeen[selectedIndex] = true;
        selected->quietFrames = 0;
        const auto amount = getDepthAmount();
        const auto timing = getTimingAmount();
        if (amount <= 0.001f) return;
        const auto wasSameFrequency = selected->frequency > 0.0f
            && std::abs(std::log2(selected->frequency / frequency)) < 0.08f;
        const auto wasRecurrence = wasSameFrequency && quietFramesBeforeEngagement >= 3;
        const auto maximumDepth = maximumSuppressionDepth(amount);
        const auto centerBin = candidateBin;
        // A pending probe owns the slot until its quick reassessment decides
        // whether it is acoustic feedback. Do not keep restarting the probe
        // merely because the shallow notch still leaves a detectable peak.
        if (wasSameFrequency && selected->awaitingConfirmation) {
            selected->calibrationHotspot =
                selected->calibrationHotspot || calibrationHotspot;
            if (calibrationHotspot) {
                // A calibrated room hotspot is already known to be unsafe.
                // Skip the shallow probe and apply the slider-defined target
                // immediately instead of waiting for raw-source confirmation.
                selected->awaitingConfirmation = false;
                selected->probeFrames = 0;
                selected->probeDepthDb = maximumDepth;
                selected->targetDepthDb = maximumDepth;
            }
            return;
        }
        // Confirmed protection remains at the requested depth while its peak
        // is present. Only a released slot can start a new probe.
        if (wasSameFrequency &&
            !selected->awaitingConfirmation &&
            (selected->persistent || selected->manual ||
             selected->targetDepthDb < -0.1f)) {
            selected->calibrationHotspot =
                selected->calibrationHotspot || calibrationHotspot;
            selected->targetDepthDb = maximumDepth;
            return;
        }
        const auto leftMagnitude = std::max(1.0e-12f,
            std::hypot(fftReal[centerBin - 1], fftImag[centerBin - 1]));
        const auto centerMagnitude = std::max(1.0e-12f,
            std::hypot(fftReal[centerBin], fftImag[centerBin]));
        const auto rightMagnitude = std::max(1.0e-12f,
            std::hypot(fftReal[centerBin + 1], fftImag[centerBin + 1]));
        // Interpolate the Hann-windowed peak in log magnitude. Linear
        // interpolation biases off-grid tones toward the nearest FFT bin.
        const auto leftLog = std::log(leftMagnitude);
        const auto centerLog = std::log(centerMagnitude);
        const auto rightLog = std::log(rightMagnitude);
        const auto curvature = leftLog - 2.0f * centerLog + rightLog;
        const auto interpolation = std::abs(curvature) > 1.0e-6f
            ? std::clamp(0.5f * (leftLog - rightLog) / curvature, -0.5f, 0.5f)
            : 0.0f;
        selected->calibrationHotspot =
            calibrationHotspot;
        selected->persistent = false;
        selected->manual = false;
        selected->frequency = std::clamp(static_cast<float>(
            (static_cast<float>(centerBin) + interpolation) * sampleRate / fftSize), 40.0f,
            static_cast<float>(sampleRate * 0.45));
        selected->q = notchQualityForCalibrationHotspot(
            selected->frequency, preset.load(std::memory_order_relaxed),
            calibrationHotspot);
        selected->probeWasRecurrence = wasRecurrence;
        selected->probeFrames = 0;
        selected->probeReferenceLevelDb = levelDb;
        // A calibrated hotspot has already been identified during measurement,
        // so it receives the full slider-defined target immediately. Other
        // candidates use the shallower probe and raw-source reassessment.
        selected->probeDepthDb = maximumDepth *
            probeDepthFraction(selected->frequency);
        selected->releaseHoldFrames = static_cast<int>(std::lround(
            10.0f + 50.0f * timing +
            (calibrationHotspot ? 24.0f + 48.0f * timing : 0.0f)));
        selected->awaitingConfirmation = !calibrationHotspot;
        selected->targetDepthDb = calibrationHotspot
            ? maximumDepth : selected->probeDepthDb;
        if (calibrationHotspot)
            selected->probeDepthDb = maximumDepth;
    }
    static constexpr std::size_t fftSize = 4096;
    static constexpr int probeConfirmationFrames = 16;
    static constexpr std::size_t analysisHop = 256;
    double sampleRate = 48000;
    int detectorSamples = 0;
    float wetMix = 0.0f;
    std::array<float, fftSize> analysisBuffer {}, fftReal {}, fftImag {};
    std::array<unsigned char, fftSize / 2> persistence {};
    std::array<unsigned char, fftSize / 2> repeatHits {};
    std::array<unsigned short, fftSize / 2> probeCooldown {};
    std::array<unsigned short, fftSize / 2> repeatHitAge {};
    std::array<unsigned char, fftSize / 2> growthFrames {};
    std::array<float, fftSize / 2> previousLevel {};
    std::array<bool, maxNotches> notchSeen {};
    std::array<float, analyzerBins> spectrumDb {};
    std::array<ShadowNotch, maxNotches> analysisNotches {};
    // Keep the large cross-thread handoff off the object stack. Windows test
    // executables commonly have a 1 MB default stack, while several
    // FeedbackProcessor instances can coexist in the DSP regression suite.
    std::unique_ptr<SpscRingBuffer<1u << 16>> inputRing =
        std::make_unique<SpscRingBuffer<1u << 16>>();
    std::array<std::atomic<float>, analyzerBins> baseline {};
    std::array<std::atomic<float>, analyzerBins> calibrationPeakBias {};
    std::array<Notch, maxNotches> states {};
    std::array<std::atomic<float>, analyzerBins> publishedSpectrum {};
    std::array<std::atomic<float>, maxNotches> publishedFrequency {}, publishedDepth {}, publishedQ {};
    std::array<std::atomic<float>, maxNotches> targetFrequency {}, targetDepth {}, targetQ {};
    std::array<std::atomic<float>, maxNotches> targetSinW {}, targetNegTwoCosW {};
    std::array<std::atomic<bool>, maxNotches> targetHotspot {};
    std::array<std::atomic<float>, maxNotches> manualNotchFrequency {};
    std::array<std::atomic<unsigned int>, maxNotches> targetSequence {};
    std::atomic<std::size_t> notchCapacity { defaultNotchesPerRoute };
    std::atomic<unsigned long long> notchResetGeneration { 0 };
    std::atomic<unsigned long long> publishedNotchResetGeneration { 0 };
    unsigned long long appliedNotchResetGeneration = 0;
    unsigned long long analyzedNotchResetGeneration = 0;
    std::atomic_bool enabled { false };
    std::atomic<ProtectionPreset> preset { ProtectionPreset::speech };
    std::atomic<float> depthAmount { 0.75f };
    std::atomic<float> sensitivityAmount { 0.75f };
    std::atomic<float> timingAmount { 0.5f };
    std::atomic<float> latchAmount { 0.5f };
};

} // namespace werfeed