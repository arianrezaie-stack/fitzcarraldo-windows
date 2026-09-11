#pragma once

#include "Dsp.h"

#include <juce_core/juce_core.h>

#include <map>

namespace werfeed {

struct CalibrationBaseline {
    int delaySamples = 0;
    std::array<float, analyzerBins> responseDb {};
};

inline void loadCalibrationBaselines(
    const juce::File& file,
    std::map<juce::String, CalibrationBaseline>& baselines) {
    const auto parsed = juce::JSON::parse(file);
    auto* root = parsed.getDynamicObject();
    if (!root) return;

    for (const auto& property : root->getProperties()) {
        auto* object = property.value.getDynamicObject();
        auto* response = object ? object->getProperty("responseDb").getArray() : nullptr;
        if (!object || !response || response->size() != static_cast<int>(analyzerBins)) continue;

        CalibrationBaseline baseline;
        baseline.delaySamples = static_cast<int>(object->getProperty("delaySamples"));
        for (std::size_t i = 0; i < analyzerBins; ++i) {
            baseline.responseDb[i] = static_cast<float>(
                static_cast<double>(response->getReference(static_cast<int>(i))));
        }
        baselines[property.name.toString()] = baseline;
    }
}

inline bool saveCalibrationBaselines(
    const juce::File& file,
    const std::map<juce::String, CalibrationBaseline>& baselines) {
    auto* root = new juce::DynamicObject();
    for (const auto& [key, baseline] : baselines) {
        auto* object = new juce::DynamicObject();
        object->setProperty("delaySamples", baseline.delaySamples);
        juce::Array<juce::var> response;
        for (const auto value : baseline.responseDb) response.add(value);
        object->setProperty("responseDb", juce::var(response));
        root->setProperty(key, juce::var(object));
    }

    if (!file.getParentDirectory().createDirectory()) return false;
    return file.replaceWithText(juce::JSON::toString(juce::var(root), true));
}

} // namespace werfeed