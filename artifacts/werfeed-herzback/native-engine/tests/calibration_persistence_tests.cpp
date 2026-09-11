#include "CalibrationPersistence.h"

#include <iostream>

namespace {

werfeed::CalibrationBaseline baseline(int delaySamples, float firstResponse) {
    werfeed::CalibrationBaseline result;
    result.delaySamples = delaySamples;
    result.responseDb.fill(firstResponse);
    result.responseDb[1] = firstResponse + 1.0f;
    return result;
}

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << "calibration-persistence-tests: " << message << '\n';
    return condition;
}

bool requireBaseline(const std::map<juce::String, werfeed::CalibrationBaseline>& baselines,
                     const juce::String& key,
                     const werfeed::CalibrationBaseline& expected,
                     const char* message) {
    const auto found = baselines.find(key);
    return require(found != baselines.end(), message) &&
        require(found->second.delaySamples == expected.delaySamples, message) &&
        require(found->second.responseDb == expected.responseDb, message);
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
    if (!require(werfeed::saveCalibrationBaselines(file, saved),
                 "could not save the initial calibration file")) return 1;

    // This is the native-engine restart boundary: create a fresh map and load
    // the same persistent file that a new Engine instance reads in its ctor.
    std::map<juce::String, werfeed::CalibrationBaseline> afterFirstRestart;
    werfeed::loadCalibrationBaselines(file, afterFirstRestart);
    if (!require(afterFirstRestart.size() == 2,
                 "initial restart did not load both route baselines")) return 1;
    if (!requireBaseline(afterFirstRestart, routeOneKey, originalRouteOne,
                         "initial restart did not preserve route one")) return 1;
    if (!requireBaseline(afterFirstRestart, routeTwoKey, originalRouteTwo,
                         "initial restart did not preserve route two")) return 1;

    // Reset only route one and persist the deletion. Route two must remain.
    afterFirstRestart.erase(routeOneKey);
    if (!require(werfeed::saveCalibrationBaselines(file, afterFirstRestart),
                 "could not save the reset calibration file")) return 1;

    std::map<juce::String, werfeed::CalibrationBaseline> afterResetRestart;
    werfeed::loadCalibrationBaselines(file, afterResetRestart);
    if (!require(afterResetRestart.size() == 1,
                 "reset restart did not retain exactly one route baseline")) return 1;
    if (!require(afterResetRestart.count(routeOneKey) == 0,
                 "reset restart retained the removed route baseline")) return 1;
    if (!requireBaseline(afterResetRestart, routeTwoKey, originalRouteTwo,
                         "reset restart did not preserve the untouched route")) return 1;

    // Recalibrating the reset route writes a new baseline without touching the
    // still-calibrated route.
    const auto recalibratedRouteOne = baseline(252, -6.0f);
    afterResetRestart[routeOneKey] = recalibratedRouteOne;
    if (!require(werfeed::saveCalibrationBaselines(file, afterResetRestart),
                 "could not save the recalibrated route baseline")) return 1;

    std::map<juce::String, werfeed::CalibrationBaseline> afterRecalibrationRestart;
    werfeed::loadCalibrationBaselines(file, afterRecalibrationRestart);
    if (!require(afterRecalibrationRestart.size() == 2,
                 "recalibration restart did not load both route baselines")) return 1;
    if (!requireBaseline(afterRecalibrationRestart, routeOneKey, recalibratedRouteOne,
                         "recalibration restart did not preserve route one")) return 1;
    if (!requireBaseline(afterRecalibrationRestart, routeTwoKey, originalRouteTwo,
                         "recalibration restart changed the untouched route")) return 1;

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
    if (!require(werfeed::saveCalibrationBaselines(malformedFile, validRouteOnly),
                 "could not save the valid malformed-route fixture")) return 1;

    auto validRoot = juce::JSON::parse(malformedFile);
    auto* validRootObject = validRoot.getDynamicObject();
    if (!require(validRootObject != nullptr,
                 "could not parse the valid malformed-route fixture")) return 1;
    auto* malformedRoute = new juce::DynamicObject();
    juce::Array<juce::var> malformedResponse;
    malformedResponse.add(-1.0f);
    malformedRoute->setProperty("delaySamples", 999);
    malformedRoute->setProperty("responseDb", juce::var(malformedResponse));
    auto* malformedRootObject = new juce::DynamicObject();
    malformedRootObject->setProperty(routeTwoKey, juce::var(malformedRoute));
    malformedRootObject->setProperty(routeOneKey, validRootObject->getProperty(routeOneKey));
    if (!require(malformedFile.replaceWithText(
                     juce::JSON::toString(juce::var(malformedRootObject), true)),
                 "could not write the malformed-route fixture")) return 1;

    std::map<juce::String, werfeed::CalibrationBaseline> afterMalformedRestart;
    werfeed::loadCalibrationBaselines(malformedFile, afterMalformedRestart);
    if (!require(afterMalformedRestart.size() == 1,
                 "malformed route prevented the valid baseline from loading")) return 1;
    if (!requireBaseline(afterMalformedRestart, routeOneKey, originalRouteOne,
                         "malformed route changed the valid baseline")) return 1;
    // A skipped record is absent, which is the native engine's uncalibrated state.
    if (!require(afterMalformedRestart.count(routeTwoKey) == 0,
                 "malformed route was loaded instead of skipped")) return 1;

    // Saving after the damaged record was skipped must preserve the valid route.
    if (!require(werfeed::saveCalibrationBaselines(malformedFile, afterMalformedRestart),
                 "could not save after skipping the malformed route")) return 1;
    std::map<juce::String, werfeed::CalibrationBaseline> afterMalformedSave;
    werfeed::loadCalibrationBaselines(malformedFile, afterMalformedSave);
    if (!require(afterMalformedSave.size() == 1,
                 "restart after malformed save did not retain one route")) return 1;
    if (!requireBaseline(afterMalformedSave, routeOneKey, originalRouteOne,
                         "restart after malformed save changed the valid baseline")) return 1;
    if (!require(afterMalformedSave.count(routeTwoKey) == 0,
                 "restart after malformed save restored the malformed route")) return 1;
}