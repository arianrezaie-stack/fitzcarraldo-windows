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
constexpr std::size_t analyzerBins = 96;
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
    for (std::size_t bin = 0; bin < analyzerBins; ++bin) {
        const auto position = static_cast<float>(bin) / static_cast<float>(analyzerBins - 1);
        const auto frequency = 20.0f * std::pow(1000.0f, position);
        double exReal = 0, exImag = 0, reReal = 0, reImag = 0;
        const auto count = std::min(excitation.size(), usable.size());
        for (std::size_t n = 0; n < count; ++n) {
            const auto phase = 2.0 * pi * frequency * static_cast<float>(n) / static_cast<float>(sampleRate);
            const auto c = std::cos(phase), s = -std::sin(phase);
            exReal += excitation[n] * c; exImag += excitation[n] * s;
            reReal += usable[n] * c; reImag += usable[n] * s;
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
    void setBaseline(const std::array<float, analyzerBins>& value) noexcept {
        for (std::size_t i = 0; i < analyzerBins; ++i)
            baseline[i].store(value[i], std::memory_order_relaxed);
    }

    float process(float sample) noexcept {
        analyze(sample);
        auto wet = sample;
        for (auto& state : states) wet = state.process(wet);
        const auto targetMix = isEnabled() ? 1.0f : 0.0f;
        wetMix += (targetMix - wetMix) * 0.0015f;
        return sample + wetMix * (wet - sample);
    }

    ProtectionSnapshot snapshot() const noexcept {
        ProtectionSnapshot result;
        for (std::size_t i = 0; i < analyzerBins; ++i)
            result.spectrumDb[i] = publishedSpectrum[i].load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < maxNotches; ++i) {
            const auto frequency = publishedFrequency[i].load(std::memory_order_relaxed);
            const auto depth = publishedDepth[i].load(std::memory_order_relaxed);
            const auto q = publishedQ[i].load(std::memory_order_relaxed);
            result.notches[i] = { frequency, depth, q, depth < -0.1f };
            if (result.notches[i].active) {
                ++result.activeNotches;
                result.maximumCutDb = std::min(result.maximumCutDb, depth);
            }
        }
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
        const auto engageAboveBaseline = selectedPreset == ProtectionPreset::speech ? 13.0f : 16.0f;
        const auto releaseAboveBaseline = engageAboveBaseline - 5.0f;
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
        const auto requiredFrames = selectedPreset == ProtectionPreset::speech ? 3 : 8;
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
            const auto left = 20.0f * std::log10(4.0f * std::hypot(fftReal[fftBin - 2], fftImag[fftBin - 2]) /
                                                 static_cast<float>(fftSize) + 1.0e-9f);
            const auto right = 20.0f * std::log10(4.0f * std::hypot(fftReal[fftBin + 2], fftImag[fftBin + 2]) /
                                                  static_cast<float>(fftSize) + 1.0e-9f);
            const auto excess = level - baseline[baselineBin].load(std::memory_order_relaxed);
            const auto tonal = level - 0.5f * (left + right);
            if (excess >= engageAboveBaseline && tonal >= 1.5f) {
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
                [candidate](std::size_t other) { return std::abs(static_cast<int>(other) - static_cast<int>(candidate)) < 4; });
            if (distinct) {
                engagedBins[engagedCount++] = candidate;
                engageFrequency(static_cast<float>(candidate * sampleRate / fftSize), selectedPreset);
            }
        }
        for (auto& notch : states) {
            if (notch.frequency <= 0 || notch.targetDepthDb >= -0.1f) continue;
            const auto fftBin = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::lround(notch.frequency * fftSize / sampleRate)), 1, fftSize / 2 - 1);
            const auto level = 20.0f * std::log10(
                4.0f * std::hypot(fftReal[fftBin], fftImag[fftBin]) / static_cast<float>(fftSize) + 1.0e-9f);
            const auto logPosition = std::log10(notch.frequency / 20.0f) / std::log10(1000.0f);
            const auto baselineBin = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::lround(logPosition * (analyzerBins - 1))), 0, analyzerBins - 1);
            if (level - baseline[baselineBin].load(std::memory_order_relaxed) < releaseAboveBaseline)
                notch.targetDepthDb = std::min(0.0f, notch.targetDepthDb + 0.25f);
        }
        for (std::size_t i = 0; i < analyzerBins; ++i)
            publishedSpectrum[i].store(spectrumDb[i], std::memory_order_relaxed);
        for (std::size_t i = 0; i < maxNotches; ++i) {
            publishedFrequency[i].store(states[i].frequency, std::memory_order_relaxed);
            publishedDepth[i].store(states[i].currentDepthDb, std::memory_order_relaxed);
            publishedQ[i].store(states[i].q, std::memory_order_relaxed);
        }
    }
    void engageFrequency(float frequency, ProtectionPreset selectedPreset) noexcept {
        Notch* selected = nullptr;
        for (auto& notch : states)
            if (notch.frequency > 0 && std::abs(std::log2(notch.frequency / frequency)) < 0.08f)
                selected = &notch;
        if (!selected)
            selected = &*std::max_element(states.begin(), states.end(),
                [](const Notch& a, const Notch& b) { return a.targetDepthDb < b.targetDepthDb; });
        selected->sampleRate = sampleRate;
        selected->frequency = std::clamp(frequency, 40.0f,
            static_cast<float>(sampleRate * 0.45));
        selected->q = selectedPreset == ProtectionPreset::speech ? 10.0f : 14.0f;
        selected->targetDepthDb = std::max(selected->targetDepthDb - 2.0f,
            selectedPreset == ProtectionPreset::speech ? -12.0f : -9.0f);
    }
    static constexpr std::size_t fftSize = 2048;
    double sampleRate = 48000;
    int detectorSamples = 0;
    float wetMix = 0.0f;
    std::array<float, fftSize> analysisBuffer {}, fftReal {}, fftImag {};
    std::array<unsigned char, fftSize / 2> persistence {};
    std::array<float, analyzerBins> spectrumDb {};
    std::array<std::atomic<float>, analyzerBins> baseline {};
    std::array<Notch, maxNotches> states {};
    std::array<std::atomic<float>, analyzerBins> publishedSpectrum {};
    std::array<std::atomic<float>, maxNotches> publishedFrequency {}, publishedDepth {}, publishedQ {};
    std::atomic_bool enabled { false };
    std::atomic<ProtectionPreset> preset { ProtectionPreset::speech };
};

} // namespace werfeed