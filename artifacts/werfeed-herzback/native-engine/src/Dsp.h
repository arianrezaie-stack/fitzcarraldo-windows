#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace werfeed {

constexpr float pi = 3.14159265358979323846f;
constexpr std::size_t analyzerBins = 256;
constexpr std::size_t maxNotches = 6;

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
    float suppressionAmount = 0.75f;
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

inline std::array<float, analyzerBins> detectionBaseline(
    const std::array<float, analyzerBins>& responseDb) noexcept {
    auto sorted = responseDb;
    std::sort(sorted.begin(), sorted.end());
    const auto median = sorted[sorted.size() / 2];
    std::array<float, analyzerBins> result {};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = -55.0f + std::clamp(responseDb[i] - median, -12.0f, 12.0f);
    return result;
}

class FeedbackProcessor {
public:
    void prepare(double newSampleRate) noexcept {
        sampleRate = std::max(8000.0, newSampleRate);
        reset();
    }
    void clearBaseline() noexcept {
        for (auto& value : baseline) value.store(-55.0f, std::memory_order_relaxed);
    }
    void reset() noexcept {
        detectorSamples = 0;
        analysisBuffer.fill(0.0f);
        persistence.fill(0);
        growthFrames.fill(0);
        previousLevel.fill(-120.0f);
        notchSeen.fill(false);
        states = {};
        spectrumDb.fill(-120.0f);
        for (std::size_t i = 0; i < analyzerBins; ++i) publishedSpectrum[i].store(-120.0f);
        for (std::size_t i = 0; i < maxNotches; ++i) {
            publishedFrequency[i].store(0.0f);
            publishedDepth[i].store(0.0f);
            publishedQ[i].store(0.0f);
        }
    }
    void setEnabled(bool value) noexcept { enabled.store(value, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }
    void setPreset(ProtectionPreset value) noexcept { preset.store(value, std::memory_order_relaxed); }
    void setSuppressionAmount(float value) noexcept {
        suppressionAmount.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float getSuppressionAmount() const noexcept {
        return suppressionAmount.load(std::memory_order_relaxed);
    }
    void setBaseline(const std::array<float, analyzerBins>& value) noexcept {
        for (std::size_t i = 0; i < analyzerBins; ++i)
            baseline[i].store(value[i], std::memory_order_relaxed);
    }

    float process(float sample) noexcept {
        if (!std::isfinite(sample)) return 0.0f;
        analyze(sample);
        auto wet = sample;
        for (auto& state : states) wet = state.process(wet);
        if (!std::isfinite(wet)) wet = 0.0f;
        const auto targetMix = isEnabled() ? 1.0f : 0.0f;
        wetMix += (targetMix - wetMix) * 0.0015f;
        const auto output = sample + wetMix * (wet - sample);
        return std::isfinite(output) ? output : 0.0f;
    }

    ProtectionSnapshot snapshot() const noexcept {
        ProtectionSnapshot result;
        for (std::size_t i = 0; i < analyzerBins; ++i)
            result.spectrumDb[i] = publishedSpectrum[i].load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < maxNotches; ++i) {
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
        result.suppressionAmount = getSuppressionAmount();
        return result;
    }

private:
    struct Notch {
        float frequency = 0, q = 8, currentDepthDb = 0, targetDepthDb = 0;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        int coefficientCountdown = 0;
        double sampleRate = 48000;
        float process(float x) noexcept {
            currentDepthDb += (targetDepthDb - currentDepthDb) * 0.0008f;
            if (std::abs(currentDepthDb) < 0.005f || frequency <= 0) return x;
            if (coefficientCountdown-- <= 0) {
                coefficientCountdown = 31;
                const auto w = 2.0f * pi * frequency / static_cast<float>(sampleRate);
                const auto alpha = std::sin(w) / (2.0f * q);
                const auto gain = std::pow(10.0f, currentDepthDb / 40.0f);
                const auto denominator = 1.0f + alpha / gain;
                b0 = (1.0f + alpha * gain) / denominator;
                b1 = -2.0f * std::cos(w) / denominator;
                b2 = (1.0f - alpha * gain) / denominator;
                a1 = -2.0f * std::cos(w) / denominator;
                a2 = (1.0f - alpha / gain) / denominator;
            }
            const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            return std::isfinite(y) ? y : 0.0f;
        }
    };

    void analyze(float sample) noexcept {
        analysisBuffer[static_cast<std::size_t>(detectorSamples)] = sample;
        if (++detectorSamples < fftSize) return;
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
        const auto engageAboveBaseline = selectedPreset == ProtectionPreset::speech ? 12.0f : 15.0f;
        notchSeen.fill(false);
        for (std::size_t bin = 0; bin < analyzerBins; ++bin) {
            const auto position = static_cast<float>(bin) / static_cast<float>(analyzerBins - 1);
            const auto frequency = 20.0f * std::pow(1000.0f, position);
            const auto fftBin = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::lround(frequency * fftSize / sampleRate)), 1, fftSize / 2 - 1);
            const auto magnitude = 4.0f * std::hypot(fftReal[fftBin], fftImag[fftBin]) /
                                   static_cast<float>(fftSize);
            spectrumDb[bin] = 20.0f * std::log10(magnitude + 1.0e-9f);
        }
        detectorSamples = 0;
        std::array<std::size_t, maxNotches> candidateBins {};
        std::array<float, maxNotches> candidateScores {};
        candidateScores.fill(-std::numeric_limits<float>::infinity());
        // The overlapping hop makes three speech frames about 16 ms apart while
        // still requiring a stable, rising tonal peak rather than a single
        // voice or music bin.
        const auto requiredFrames = selectedPreset == ProtectionPreset::speech ? 2 : 18;
        const auto firstBin = std::max<std::size_t>(2, static_cast<std::size_t>(40.0 * fftSize / sampleRate));
        const auto lastBin = std::min<std::size_t>(fftSize / 2 - 2,
            static_cast<std::size_t>(std::min(20000.0, sampleRate * 0.45) * fftSize / sampleRate));
        for (std::size_t fftBin = firstBin; fftBin <= lastBin; ++fftBin) {
            const auto frequency = static_cast<float>(fftBin * sampleRate / fftSize);
            const auto logPosition = std::log10(frequency / 20.0f) / std::log10(1000.0f);
            const auto baselineBin = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::lround(logPosition * (analyzerBins - 1))), 0, analyzerBins - 1);
            const auto magnitude = 4.0f * std::hypot(fftReal[fftBin], fftImag[fftBin]) /
                                   static_cast<float>(fftSize);
            const auto level = 20.0f * std::log10(magnitude + 1.0e-9f);
            const auto levelDelta = level - previousLevel[fftBin];
            if (levelDelta > 0.35f)
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
            // Broad speech fundamentals and harmonics are less likely to pass
            // this wider neighborhood comparison than a narrow room howl.
            const auto tonal = level - neighborhoodLevel;
            const auto tonalThreshold = selectedPreset == ProtectionPreset::music
                ? 8.0f : (frequency < 350.0f ? 7.0f : 4.0f);
            const auto leftMagnitude = 4.0f * std::hypot(fftReal[fftBin - 1], fftImag[fftBin - 1]) /
                                       static_cast<float>(fftSize);
            const auto rightMagnitude = 4.0f * std::hypot(fftReal[fftBin + 1], fftImag[fftBin + 1]) /
                                        static_cast<float>(fftSize);
            const auto localPeak = magnitude >= leftMagnitude && magnitude >= rightMagnitude;
            const auto risingPeak = growthFrames[fftBin] >= 2;
            const auto stableStrongPeak = excess >= 24.0f;
            if (localPeak && excess >= engageAboveBaseline && tonal >= tonalThreshold &&
                (risingPeak || stableStrongPeak)) {
                persistence[fftBin] = static_cast<unsigned char>(
                    std::min<int>(255, persistence[fftBin] + 1));
                if (persistence[fftBin] < requiredFrames) continue;
                const auto score = excess + tonal;
                for (std::size_t slot = 0; slot < maxNotches; ++slot) {
                    if (score <= candidateScores[slot]) continue;
                    for (std::size_t move = maxNotches - 1; move > slot; --move) {
                        candidateScores[move] = candidateScores[move - 1];
                        candidateBins[move] = candidateBins[move - 1];
                    }
                    candidateScores[slot] = score;
                    candidateBins[slot] = fftBin;
                    break;
                }
            } else {
                persistence[fftBin] = static_cast<unsigned char>(
                    persistence[fftBin] > 1 ? persistence[fftBin] - 2 : 0);
            }
        }
        std::array<std::size_t, maxNotches> engagedBins {};
        std::size_t engagedCount = 0;
        for (const auto candidate : candidateBins) {
            if (candidate == 0) continue;
            const auto distinct = std::none_of(engagedBins.begin(), engagedBins.begin() + engagedCount,
                [candidate](std::size_t other) { return std::abs(static_cast<int>(other) - static_cast<int>(candidate)) < 2; });
            if (distinct) {
                engagedBins[engagedCount++] = candidate;
                engageFrequency(static_cast<float>(candidate * sampleRate / fftSize), selectedPreset);
            }
        }
        for (std::size_t i = 0; i < states.size(); ++i) {
            auto& notch = states[i];
            if (notch.frequency <= 0 || notchSeen[i]) continue;
            // Release from candidate absence as well as level. This prevents a
            // stale low-frequency notch from re-engaging itself after the room
            // has gone quiet.
            notch.targetDepthDb = std::min(0.0f, notch.targetDepthDb + 1.5f);
            if (notch.targetDepthDb >= -0.1f && notch.currentDepthDb > -0.5f)
                notch = {};
        }
        for (std::size_t i = 0; i < analyzerBins; ++i)
            publishedSpectrum[i].store(spectrumDb[i], std::memory_order_relaxed);
        for (std::size_t i = 0; i < maxNotches; ++i) {
            publishedFrequency[i].store(states[i].frequency, std::memory_order_relaxed);
            publishedDepth[i].store(states[i].currentDepthDb, std::memory_order_relaxed);
            publishedQ[i].store(states[i].q, std::memory_order_relaxed);
        }
        std::copy(analysisBuffer.begin() + static_cast<std::ptrdiff_t>(analysisHop),
                  analysisBuffer.end(), analysisBuffer.begin());
        detectorSamples = fftSize - analysisHop;
    }
    void engageFrequency(float frequency, ProtectionPreset selectedPreset) noexcept {
        Notch* selected = nullptr;
        for (auto& notch : states) {
            if (notch.frequency > 0 && std::abs(std::log2(notch.frequency / frequency)) < 0.08f) {
                selected = &notch;
                break;
            }
        }
        if (!selected) {
            const auto freeSlot = std::find_if(states.begin(), states.end(),
                [](const Notch& notch) {
                    return notch.frequency <= 0 || (notch.targetDepthDb >= -0.1f && notch.currentDepthDb > -0.5f);
                });
            if (freeSlot != states.end()) selected = &*freeSlot;
        }
        if (!selected)
            selected = &*std::max_element(states.begin(), states.end(),
                [](const Notch& a, const Notch& b) { return a.currentDepthDb < b.currentDepthDb; });
        const auto selectedIndex = static_cast<std::size_t>(selected - states.data());
        notchSeen[selectedIndex] = true;
        const auto amount = getSuppressionAmount();
        if (amount <= 0.001f) return;
        const auto maximumDepth = -amount * 12.0f;
        const auto centerBin = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::lround(frequency * fftSize / sampleRate)),
            1, fftSize / 2 - 1);
        const auto leftMagnitude = std::hypot(fftReal[centerBin - 1], fftImag[centerBin - 1]);
        const auto centerMagnitude = std::hypot(fftReal[centerBin], fftImag[centerBin]);
        const auto rightMagnitude = std::hypot(fftReal[centerBin + 1], fftImag[centerBin + 1]);
        const auto curvature = leftMagnitude - 2.0f * centerMagnitude + rightMagnitude;
        const auto interpolation = std::abs(curvature) > 1.0e-9f
            ? std::clamp(0.5f * (leftMagnitude - rightMagnitude) / curvature, -0.5f, 0.5f)
            : 0.0f;
        selected->sampleRate = sampleRate;
        selected->frequency = std::clamp(static_cast<float>(
            (static_cast<float>(centerBin) + interpolation) * sampleRate / fftSize), 40.0f,
            static_cast<float>(sampleRate * 0.45));
        selected->q = frequency < 300.0f
            ? 2.0f + 10.0f * std::clamp(frequency / 300.0f, 0.0f, 1.0f)
            : (selectedPreset == ProtectionPreset::speech ? 18.0f : 16.0f);
        // Reach useful attenuation on the first engaged frame, then let the
        // next frames move to the route's chosen maximum cut.
        selected->targetDepthDb = std::max(selected->targetDepthDb - 4.5f, maximumDepth);
    }
    static constexpr std::size_t fftSize = 2048;
    static constexpr std::size_t analysisHop = 256;
    double sampleRate = 48000;
    int detectorSamples = 0;
    float wetMix = 0.0f;
    std::array<float, fftSize> analysisBuffer {}, fftReal {}, fftImag {};
    std::array<unsigned char, fftSize / 2> persistence {};
    std::array<unsigned char, fftSize / 2> growthFrames {};
    std::array<float, fftSize / 2> previousLevel {};
    std::array<bool, maxNotches> notchSeen {};
    std::array<float, analyzerBins> spectrumDb {};
    std::array<std::atomic<float>, analyzerBins> baseline {};
    std::array<Notch, maxNotches> states {};
    std::array<std::atomic<float>, analyzerBins> publishedSpectrum {};
    std::array<std::atomic<float>, maxNotches> publishedFrequency {}, publishedDepth {}, publishedQ {};
    std::atomic_bool enabled { false };
    std::atomic<ProtectionPreset> preset { ProtectionPreset::speech };
    std::atomic<float> suppressionAmount { 0.75f };
};

} // namespace werfeed