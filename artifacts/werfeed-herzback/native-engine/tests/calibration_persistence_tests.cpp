#include "CalibrationPersistence.h"

#include <cassert>

namespace {

werfeed::CalibrationBaseline baseline(int delaySamples, float firstResponse) {
    werfeed::CalibrationBaseline result;
    result.delaySamples = delaySamples;
    result.responseDb.fill(firstResponse);
    result.responseDb[1] = firstResponse + 1.0f;
    return result;
}

void assertSame(const werfeed::CalibrationBaseline& expected,
                const werfeed::CalibrationBaseline& actual) {
    assert(expected.delaySamples == actual.delaySamples);
    assert(expected.responseDb == actual.responseDb);
}

} // namespace

int main() {
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("werfeed-calibration-persistence", ".json");
    struct Cleanup {
        juce::File file;
        ~Cleanup() { file.deleteFile(); }
    } cleanup { file };

    const auto routeOneKey = "Windows Audio|input|output|in:0|out:0";
    const auto routeTwoKey = "Windows Audio|input|output|in:1|out:1";
    const auto originalRouteOne = baseline(240, -12.0f);
    const auto originalRouteTwo = baseline(480, -9.0f);

    std::map<juce::String, werfeed::CalibrationBaseline> saved {
        { routeOneKey, originalRouteOne },
        { routeTwoKey, originalRouteTwo },
    };
    assert(werfeed::saveCalibrationBaselines(file, saved));

    // This is the native-engine restart boundary: create a fresh map and load
    // the same persistent file that a new Engine instance reads in its ctor.
    std::map<juce::String, werfeed::CalibrationBaseline> afterFirstRestart;
    werfeed::loadCalibrationBaselines(file, afterFirstRestart);
    assert(afterFirstRestart.size() == 2);
    assertSame(originalRouteOne, afterFirstRestart.at(routeOneKey));
    assertSame(originalRouteTwo, afterFirstRestart.at(routeTwoKey));

    // Reset only route one and persist the deletion. Route two must remain.
    afterFirstRestart.erase(routeOneKey);
    assert(werfeed::saveCalibrationBaselines(file, afterFirstRestart));

    std::map<juce::String, werfeed::CalibrationBaseline> afterResetRestart;
    werfeed::loadCalibrationBaselines(file, afterResetRestart);
    assert(afterResetRestart.size() == 1);
    assert(afterResetRestart.count(routeOneKey) == 0);
    assertSame(originalRouteTwo, afterResetRestart.at(routeTwoKey));

    // Recalibrating the reset route writes a new baseline without touching the
    // still-calibrated route.
    const auto recalibratedRouteOne = baseline(252, -6.0f);
    afterResetRestart[routeOneKey] = recalibratedRouteOne;
    assert(werfeed::saveCalibrationBaselines(file, afterResetRestart));

    std::map<juce::String, werfeed::CalibrationBaseline> afterRecalibrationRestart;
    werfeed::loadCalibrationBaselines(file, afterRecalibrationRestart);
    assert(afterRecalibrationRestart.size() == 2);
    assertSame(recalibratedRouteOne, afterRecalibrationRestart.at(routeOneKey));
    assertSame(originalRouteTwo, afterRecalibrationRestart.at(routeTwoKey));

    const auto malformedFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("werfeed-calibration-malformed", ".json");
    struct MalformedCleanup {
        juce::File file;
        ~MalformedCleanup() { file.deleteFile(); }
    } malformedCleanup { malformedFile };

    // A malformed route record must not prevent a valid route from loading.
    std::map<juce::String, werfeed::CalibrationBaseline> validRouteOnly {
        { routeOneKey, originalRouteOne },
    };
    assert(werfeed::saveCalibrationBaselines(malformedFile, validRouteOnly));

    auto validRoot = juce::JSON::parse(malformedFile);
    auto* validRootObject = validRoot.getDynamicObject();
    assert(validRootObject != nullptr);
    auto* malformedRoute = new juce::DynamicObject();
    juce::Array<juce::var> malformedResponse;
    malformedResponse.add(-1.0f);
    malformedRoute->setProperty("delaySamples", 999);
    malformedRoute->setProperty("responseDb", juce::var(malformedResponse));
    auto* malformedRootObject = new juce::DynamicObject();
    malformedRootObject->setProperty(routeTwoKey, juce::var(malformedRoute));
    malformedRootObject->setProperty(routeOneKey, validRootObject->getProperty(routeOneKey));
    assert(malformedFile.replaceWithText(
        juce::JSON::toString(juce::var(malformedRootObject), true)));

    std::map<juce::String, werfeed::CalibrationBaseline> afterMalformedRestart;
    werfeed::loadCalibrationBaselines(malformedFile, afterMalformedRestart);
    assert(afterMalformedRestart.size() == 1);
    assertSame(originalRouteOne, afterMalformedRestart.at(routeOneKey));
    // A skipped record is absent, which is the native engine's uncalibrated state.
    assert(afterMalformedRestart.count(routeTwoKey) == 0);

    // Saving after the damaged record was skipped must preserve the valid route.
    assert(werfeed::saveCalibrationBaselines(malformedFile, afterMalformedRestart));
    std::map<juce::String, werfeed::CalibrationBaseline> afterMalformedSave;
    werfeed::loadCalibrationBaselines(malformedFile, afterMalformedSave);
    assert(afterMalformedSave.size() == 1);
    assertSame(originalRouteOne, afterMalformedSave.at(routeOneKey));
    assert(afterMalformedSave.count(routeTwoKey) == 0);
}