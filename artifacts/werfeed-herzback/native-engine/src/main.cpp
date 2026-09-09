#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include "Dsp.h"
#include "Routing.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <map>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex stdoutMutex;

void emit(const juce::var& event) {
    const std::lock_guard<std::mutex> guard(stdoutMutex);
    std::cout << juce::JSON::toString(event, false).toStdString() << '\n' << std::flush;
}
void error(const juce::String& message) {
    auto* o = new juce::DynamicObject();
    o->setProperty("type", "error");
    o->setProperty("code", "ENGINE_ERROR");
    o->setProperty("message", message);
    emit(juce::var(o));
}
bool getObject(const juce::var& v, juce::DynamicObject*& o) {
    o = v.getDynamicObject(); return o != nullptr;
}
bool isWasapiType(const juce::String& typeName) {
    return typeName.startsWithIgnoreCase("Windows Audio");
}

class Engine final : public juce::AudioIODeviceCallback {
public:
    Engine() {
        calibrationFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Werfeed Herzback").getChildFile("calibrations.json");
        loadCalibrations();
        const auto result = manager.initialise(0, 0, nullptr, true);
        if (result.isNotEmpty()) error(result);
    }
    ~Engine() override { manager.closeAudioDevice(); }

    void enumerate() {
        juce::Array<juce::var> devices;
        for (auto* type : manager.getAvailableDeviceTypes()) {
            if (!isWasapiType(type->getTypeName())) continue;
            type->scanForDevices();
            for (const auto& name : type->getDeviceNames(true)) addDevice(devices, *type, name, true);
            for (const auto& name : type->getDeviceNames(false)) addDevice(devices, *type, name, false);
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "devices"); o->setProperty("devices", juce::var(devices));
        emit(juce::var(o));
    }

    void configure(const juce::DynamicObject& command) {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (running.load()) { error("stop before configuring routes or devices"); return; }
        if (calibrationBusy.load()) { error("wait for calibration finalization before configuring"); return; }
        if (callbackRegistered.exchange(false)) manager.removeAudioCallback(this);
        const auto typeName = command.getProperty("deviceType").toString();
        const auto inputName = command.getProperty("inputDevice").toString();
        const auto outputName = command.getProperty("outputDevice").toString();
        if (typeName.isEmpty()) { error("configure requires deviceType"); return; }
        if (!isWasapiType(typeName)) { error("Werfeed supports WASAPI devices only"); return; }
        manager.setCurrentAudioDeviceType(typeName, true);
        if (manager.getCurrentDeviceType() == nullptr ||
            manager.getCurrentDeviceType()->getTypeName() != typeName) {
            error("audio device type unavailable"); return;
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = inputName;
        setup.outputDeviceName = outputName;
        setup.sampleRate = static_cast<double>(command.getProperty("sampleRate", 0.0));
        setup.bufferSize = static_cast<int>(command.getProperty("bufferSize", 0));
        setup.useDefaultInputChannels = false; setup.useDefaultOutputChannels = false;
        const auto inChannels = static_cast<int>(command.getProperty("inputChannels", 8));
        const auto outChannels = static_cast<int>(command.getProperty("outputChannels", 8));
        setup.inputChannels.setRange(0, juce::jlimit(0, 64, inChannels), true);
        setup.outputChannels.setRange(0, juce::jlimit(0, 64, outChannels), true);
        if (!parseRoutes(command.getProperty("routes"))) return;
        const auto result = manager.initialise(inChannels, outChannels, nullptr, true, {}, &setup);
        if (result.isNotEmpty()) { error(result); return; }
        routeBaseKey = typeName + "|" + inputName + "|" + outputName;
        for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex) {
            auto& processor = processors[static_cast<std::size_t>(routeIndex)];
            processor.prepare(manager.getCurrentAudioDevice()->getCurrentSampleRate());
            processor.clearBaseline();
            const auto found = baselines.find(keyForRoute(routeIndex));
            if (found != baselines.end()) processor.setBaseline(werfeed::detectionBaseline(found->second.responseDb));
        }
        configured.store(true);
        emitState("configured");
    }

    void start() {
        if (!configured.load()) { error("configure a device before start"); return; }
        if (!callbackRegistered.exchange(true)) manager.addAudioCallback(this);
        running.store(true);
        emitState("started");
    }
    void stop() {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        running.store(false);
        if (callbackRegistered.exchange(false)) manager.removeAudioCallback(this);
        cancelCalibration();
        emitState("stopped");
    }
    bool isRunning() const noexcept { return running.load(); }

    void setProtection(const juce::DynamicObject& command) {
        const auto shouldEnable = static_cast<bool>(command.getProperty("enabled", true));
        const auto presetName = command.getProperty("preset", "speech").toString();
        if (presetName != "speech" && presetName != "music") {
            error("protection preset must be speech or music"); return;
        }
        const auto selected = presetName == "music"
            ? werfeed::ProtectionPreset::music : werfeed::ProtectionPreset::speech;
        for (auto& processor : processors) {
            processor.setPreset(selected);
            processor.setEnabled(shouldEnable);
        }
        protectionEnabled.store(shouldEnable);
        protectionPreset.store(selected);
        emitState("protection_changed");
    }

    void startCalibration(const juce::DynamicObject& command) {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (!running.load() || !deviceActive.load()) { error("start active audio before calibration"); return; }
        if (calibrationBusy.exchange(true)) { error("calibration is already running or finalizing"); return; }
        const auto route = static_cast<int>(command.getProperty("route", 0));
        if (route < 0 || route >= routeCount) { calibrationBusy.store(false); error("calibration route is invalid"); return; }
        const auto level = static_cast<float>(static_cast<double>(command.getProperty("level", 0.06)));
        if (!(level > 0.0f && level <= 0.08f)) { calibrationBusy.store(false); error("calibration level must be above 0 and at most 0.08"); return; }
        const auto rate = sampleRate.load();
        if (rate < 8000.0) { calibrationBusy.store(false); error("audio device sample rate is unavailable"); return; }
        const auto impulseLength = static_cast<std::size_t>(rate * 0.5);
        const auto gapLength = static_cast<std::size_t>(rate * 0.25);
        const auto probe = werfeed::makeDelayProbe(level);
        auto sweep = werfeed::makeLogSweep(rate, 2.0, 20.0f,
            static_cast<float>(std::min(20000.0, rate * 0.45)), level);
        calibrationExcitation.assign(impulseLength + gapLength + sweep.size(), 0.0f);
        std::copy(probe.begin(), probe.end(), calibrationExcitation.begin());
        std::copy(sweep.begin(), sweep.end(),
                  calibrationExcitation.begin() + static_cast<std::ptrdiff_t>(impulseLength + gapLength));
        calibrationRecording.assign(calibrationExcitation.size() + static_cast<std::size_t>(rate), 0.0f);
        calibrationRoute = route;
        calibrationRouteKey = keyForRoute(route);
        calibrationGeneration = generationCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        calibrationPosition.store(0);
        calibrationComplete.store(false);
        calibrating.store(true, std::memory_order_release);
        emitState("calibrating");
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        sampleRate.store(device->getCurrentSampleRate());
        bufferSize.store(device->getCurrentBufferSizeSamples());
        for (auto& processor : processors) processor.prepare(device->getCurrentSampleRate());
        deviceActive.store(true);
    }
    void audioDeviceStopped() override {
        deviceActive.store(false);
        running.store(false);
        deviceStopPending.store(true);
        cancelCalibration();
    }
    void audioDeviceIOCallbackWithContext(const float* const* input, int ins,
                                          float* const* output, int outs, int samples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        const auto begun = std::chrono::steady_clock::now();
        for (int channel = 0; channel < outs; ++channel) std::fill_n(output[channel], samples, 0.0f);
        const auto calibrationActive = calibrating.load(std::memory_order_acquire);
        auto calibrationIndex = calibrationPosition.load(std::memory_order_relaxed);
        if (calibrationActive) {
            werfeed::routeCalibration(input, ins, output, outs, samples,
                routes[static_cast<std::size_t>(calibrationRoute)],
                calibrationExcitation, calibrationRecording, calibrationIndex);
        } else {
            for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex) {
                const auto route = routes[static_cast<std::size_t>(routeIndex)];
                if (route.input < 0 || route.input >= ins || route.output < 0 || route.output >= outs) continue;
                for (int frame = 0; frame < samples; ++frame) {
                    const auto value = processors[static_cast<std::size_t>(routeIndex)].process(input[route.input][frame]);
                    output[route.output][frame] += value;
                }
            }
        }
        if (calibrationActive) {
            calibrationIndex += static_cast<std::size_t>(samples);
            calibrationPosition.store(calibrationIndex, std::memory_order_relaxed);
            if (calibrationIndex >= calibrationRecording.size()) {
                calibrating.store(false, std::memory_order_release);
                completedGeneration.store(calibrationGeneration, std::memory_order_relaxed);
                calibrationComplete.store(true, std::memory_order_release);
            }
        }
        float peakIn = 0.0f, peakOut = 0.0f;
        for (int c = 0; c < ins; ++c) for (int n = 0; n < samples; ++n) peakIn = std::max(peakIn, std::abs(input[c][n]));
        for (int c = 0; c < outs; ++c) for (int n = 0; n < samples; ++n) peakOut = std::max(peakOut, std::abs(output[c][n]));
        inputPeak.store(peakIn, std::memory_order_relaxed); outputPeak.store(peakOut, std::memory_order_relaxed);
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count();
        const auto budget = samples / sampleRate.load();
        cpu.store(budget > 0.0 ? elapsed / budget : 0.0, std::memory_order_relaxed);
        if (elapsed > budget) xruns.fetch_add(1, std::memory_order_relaxed);
    }

    void emitState(const char* phase) {
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "state");
        o->setProperty("phase", phase);
        o->setProperty("running", running.load());
        o->setProperty("sampleRate", sampleRate.load()); o->setProperty("bufferSize", bufferSize.load());
        emit(juce::var(o));
    }

    void emitTelemetry() {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "telemetry");
        o->setProperty("running", running.load());
        o->setProperty("sampleRate", sampleRate.load()); o->setProperty("bufferSize", bufferSize.load());
        o->setProperty("callbackCpu", cpu.load()); o->setProperty("xruns", static_cast<double>(xruns.load()));
        o->setProperty("inputPeak", inputPeak.load()); o->setProperty("outputPeak", outputPeak.load());
        o->setProperty("protectionEnabled", protectionEnabled.load());
        o->setProperty("preset", protectionPreset.load() == werfeed::ProtectionPreset::music ? "music" : "speech");
        o->setProperty("calibrating", calibrating.load());
        juce::Array<juce::var> calibratedRoutes;
        for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex)
            calibratedRoutes.add(baselines.contains(keyForRoute(routeIndex)));
        o->setProperty("calibratedRoutes", juce::var(calibratedRoutes));
        o->setProperty("calibrated", routeCount > 0 && baselines.contains(keyForRoute(0)));
        juce::Array<juce::var> spectrum;
        juce::Array<juce::var> notches;
        int activeNotches = 0;
        float maximumCut = 0.0f;
        if (routeCount > 0) {
            const auto snapshot = processors[0].snapshot();
            for (const auto value : snapshot.spectrumDb) spectrum.add(value);
            for (const auto& notch : snapshot.notches) if (notch.active) {
                auto* n = new juce::DynamicObject();
                n->setProperty("frequency", notch.frequency);
                n->setProperty("depthDb", notch.depthDb);
                n->setProperty("q", notch.q);
                notches.add(juce::var(n));
            }
            activeNotches = snapshot.activeNotches;
            maximumCut = snapshot.maximumCutDb;
        }
        o->setProperty("spectrumDb", juce::var(spectrum));
        o->setProperty("notches", juce::var(notches));
        o->setProperty("activeNotches", activeNotches);
        o->setProperty("maximumCutDb", maximumCut);
        emit(juce::var(o));
    }

    void finishCalibrationIfReady() {
        if (!calibrationComplete.exchange(false, std::memory_order_acq_rel)) return;
        const auto generation = completedGeneration.load(std::memory_order_relaxed);
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (generation != generationCounter.load(std::memory_order_relaxed)) return;
        const auto rate = sampleRate.load();
        constexpr std::size_t probeLength = 2047;
        const auto impulseLength = static_cast<std::size_t>(rate * 0.5);
        const auto gapLength = static_cast<std::size_t>(rate * 0.25);
        const auto delay = werfeed::estimateDelay(
            std::span<const float>(calibrationExcitation.data(), probeLength),
            calibrationRecording, static_cast<int>(rate * 0.5), 0.2f);
        if (delay < 0) {
            calibrationBusy.store(false, std::memory_order_release);
            error("calibration failed: delay probe was not detected safely");
            emitState("calibration_failed"); return;
        }
        const auto sweepOffset = impulseLength + gapLength;
        double noisePower = 0.0, sweepPower = 0.0;
        const auto noiseStart = impulseLength;
        const auto noiseEnd = sweepOffset;
        for (auto i = noiseStart; i < noiseEnd; ++i)
            noisePower += calibrationRecording[i] * calibrationRecording[i];
        const auto recordedSweepStart = sweepOffset + static_cast<std::size_t>(delay);
        const auto recordedSweepCount = std::min(
            calibrationExcitation.size() - sweepOffset,
            calibrationRecording.size() - recordedSweepStart);
        for (std::size_t i = 0; i < recordedSweepCount; ++i) {
            const auto value = calibrationRecording[recordedSweepStart + i];
            sweepPower += value * value;
        }
        const auto noiseRms = std::sqrt(noisePower / std::max<std::size_t>(1, noiseEnd - noiseStart));
        const auto sweepRms = std::sqrt(sweepPower / std::max<std::size_t>(1, recordedSweepCount));
        if (recordedSweepCount == 0 || sweepRms < 1.0e-5 || sweepRms < noiseRms * 3.0) {
            calibrationBusy.store(false, std::memory_order_release);
            error("calibration failed: sweep response signal-to-noise ratio is too low");
            emitState("calibration_failed"); return;
        }
        const auto response = werfeed::measureResponse(
            std::span<const float>(calibrationExcitation).subspan(sweepOffset),
            std::span<const float>(calibrationRecording).subspan(sweepOffset),
            rate, delay);
        const auto calibrationKey = calibrationRouteKey;
        baselines[calibrationKey] = { delay, response };
        const auto detectorBaseline = werfeed::detectionBaseline(response);
        processors[static_cast<std::size_t>(calibrationRoute)].setBaseline(detectorBaseline);
        if (!saveCalibrations()) error("calibration completed but its baseline could not be persisted");
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "calibration");
        o->setProperty("routeKey", calibrationKey);
        o->setProperty("delaySamples", delay);
        o->setProperty("delayMs", delay * 1000.0 / rate);
        juce::Array<juce::var> curve;
        for (const auto value : response) curve.add(value);
        o->setProperty("responseDb", juce::var(curve));
        emit(juce::var(o));
        emitState("calibrated");
        calibrationBusy.store(false, std::memory_order_release);
    }
    void reportLifecycle() {
        if (deviceStopPending.exchange(false) && !running.load()) emitState("device_stopped");
    }

private:
    struct Baseline {
        int delaySamples = 0;
        std::array<float, werfeed::analyzerBins> responseDb {};
    };
    void loadCalibrations() {
        const auto parsed = juce::JSON::parse(calibrationFile);
        auto* root = parsed.getDynamicObject();
        if (!root) return;
        for (const auto& property : root->getProperties()) {
            auto* object = property.value.getDynamicObject();
            auto* response = object ? object->getProperty("responseDb").getArray() : nullptr;
            if (!object || !response || response->size() != static_cast<int>(werfeed::analyzerBins)) continue;
            Baseline baseline;
            baseline.delaySamples = static_cast<int>(object->getProperty("delaySamples"));
            for (std::size_t i = 0; i < werfeed::analyzerBins; ++i)
                baseline.responseDb[i] = static_cast<float>(static_cast<double>(response->getReference(static_cast<int>(i))));
            baselines[property.name.toString()] = baseline;
        }
    }
    bool saveCalibrations() {
        auto* root = new juce::DynamicObject();
        for (const auto& [key, baseline] : baselines) {
            auto* object = new juce::DynamicObject();
            object->setProperty("delaySamples", baseline.delaySamples);
            juce::Array<juce::var> response;
            for (const auto value : baseline.responseDb) response.add(value);
            object->setProperty("responseDb", juce::var(response));
            root->setProperty(key, juce::var(object));
        }
        if (!calibrationFile.getParentDirectory().createDirectory()) return false;
        return calibrationFile.replaceWithText(juce::JSON::toString(juce::var(root), true));
    }
    juce::String keyForRoute(int routeIndex) const {
        if (routeIndex < 0 || routeIndex >= routeCount) return {};
        const auto route = routes[static_cast<std::size_t>(routeIndex)];
        return routeBaseKey + "|in:" + juce::String(route.input) + "|out:" + juce::String(route.output);
    }
    void cancelCalibration() noexcept {
        if (calibrationBusy.exchange(false, std::memory_order_acq_rel)) {
            generationCounter.fetch_add(1, std::memory_order_relaxed);
            calibrating.store(false, std::memory_order_release);
            calibrationComplete.store(false, std::memory_order_release);
            calibrationPosition.store(0, std::memory_order_relaxed);
        }
    }
    void addDevice(juce::Array<juce::var>& devices, juce::AudioIODeviceType& type, const juce::String& name, bool input) {
        auto* d = new juce::DynamicObject();
        d->setProperty("deviceType", type.getTypeName()); d->setProperty("name", name);
        d->setProperty("direction", input ? "input" : "output"); devices.add(juce::var(d));
    }
    bool parseRoutes(const juce::var& value) {
        auto* array = value.getArray();
        if (array == nullptr || array->size() > static_cast<int>(werfeed::maxRoutes)) { error("routes must be an array of at most 8 pairs"); return false; }
        for (int i = 0; i < array->size(); ++i) {
            auto* r = array->getReference(i).getDynamicObject();
            if (!r) { error("each route must be an object"); return false; }
            routes[static_cast<size_t>(i)] = { static_cast<int>(r->getProperty("input")), static_cast<int>(r->getProperty("output")) };
        }
        routeCount = array->size(); return true;
    }
    juce::AudioDeviceManager manager;
    std::array<werfeed::Route, werfeed::maxRoutes> routes {};
    std::array<werfeed::FeedbackProcessor, werfeed::maxRoutes> processors {};
    int routeCount = 0; // only changed while callback is detached
    std::atomic_bool configured { false }, running { false };
    std::atomic<double> sampleRate { 0.0 }, cpu { 0.0 };
    std::atomic<int> bufferSize { 0 };
    std::atomic<unsigned long long> xruns { 0 };
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
    std::atomic_bool protectionEnabled { false }, calibrating { false }, calibrationComplete { false };
    std::atomic_bool calibrationBusy { false }, deviceActive { false }, deviceStopPending { false };
    std::atomic_bool callbackRegistered { false };
    std::atomic<werfeed::ProtectionPreset> protectionPreset { werfeed::ProtectionPreset::speech };
    std::atomic<std::size_t> calibrationPosition { 0 };
    std::atomic<unsigned long long> generationCounter { 0 }, completedGeneration { 0 };
    unsigned long long calibrationGeneration = 0;
    int calibrationRoute = 0;
    std::vector<float> calibrationExcitation, calibrationRecording;
    juce::File calibrationFile;
    juce::String routeBaseKey;
    juce::String calibrationRouteKey;
    std::map<juce::String, Baseline> baselines;
    std::mutex controlMutex;
};

} // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceRuntime;
    Engine engine;
    {
        auto* hello = new juce::DynamicObject();
        hello->setProperty("type", "hello");
        hello->setProperty("protocolVersion", 1);
        hello->setProperty("engineVersion", "0.1.0");
        emit(juce::var(hello));
    }
    std::atomic_bool done { false };
    std::thread reporter([&] { while (!done.load()) { engine.reportLifecycle(); engine.finishCalibrationIfReady(); if (engine.isRunning()) engine.emitTelemetry(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); } });
    std::string line;
    while (std::getline(std::cin, line)) {
        juce::var command; const auto result = juce::JSON::parse(juce::String(line), command);
        juce::DynamicObject* object = nullptr;
        if (result.failed() || !getObject(command, object)) { error("expected one JSON object per line"); continue; }
        const auto name = object->getProperty("type").toString();
        if (name == "list_devices") engine.enumerate();
        else if (name == "configure") engine.configure(*object);
        else if (name == "start") engine.start();
        else if (name == "stop") engine.stop();
        else if (name == "set_protection") engine.setProtection(*object);
        else if (name == "start_calibration") engine.startCalibration(*object);
        else error("unknown command");
    }
    engine.stop(); done.store(true); reporter.join();
}