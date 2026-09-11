#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "Dsp.h"
#include "DevicePolicy.h"
#include "Routing.h"
#include "CalibrationPersistence.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex stdoutMutex;

class StderrLogger final : public juce::Logger {
public:
    void logMessage(const juce::String& message) override {
        std::cerr << message.toStdString() << '\n' << std::flush;
    }
};

void emit(const juce::var& event) {
    const std::lock_guard<std::mutex> guard(stdoutMutex);
    // The Electron bridge consumes stdout one line at a time. JUCE's
    // allOnOneLine flag must be true or a pretty-printed event is split into
    // invalid partial JSON messages.
    std::cout << juce::JSON::toString(event, true).toStdString() << '\n' << std::flush;
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
juce::var getPropertyOr(const juce::DynamicObject& object, const char* name, juce::var fallback) {
    const juce::Identifier propertyName(name);
    return object.hasProperty(propertyName) ? object.getProperty(propertyName) : fallback;
}

class Engine final : public juce::AudioIODeviceCallback {
public:
    Engine() {
        routeDepth.fill(0.75f);
        routeSensitivity.fill(0.75f);
        routeTiming.fill(0.5f);
        routeLatch.fill(0.5f);
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
        manager.setCurrentAudioDeviceType(typeName, true);
        if (manager.getCurrentAudioDeviceType() != typeName) {
            error("audio device type unavailable"); return;
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = inputName;
        setup.outputDeviceName = outputName;
        setup.sampleRate = static_cast<double>(getPropertyOr(command, "sampleRate", 0.0));
        setup.bufferSize = static_cast<int>(getPropertyOr(command, "bufferSize", 0));
        setup.useDefaultInputChannels = false; setup.useDefaultOutputChannels = false;
        const auto inChannels = static_cast<int>(getPropertyOr(command, "inputChannels", 8));
        const auto outChannels = static_cast<int>(getPropertyOr(command, "outputChannels", 8));
        setup.inputChannels.setRange(0, juce::jlimit(0, 64, inChannels), true);
        setup.outputChannels.setRange(0, juce::jlimit(0, 64, outChannels), true);
        if (!parseRoutes(command.getProperty("routes"), inChannels, outChannels)) return;
        const auto result = manager.initialise(inChannels, outChannels, nullptr, true, {}, &setup);
        if (result.isNotEmpty()) { error(result); return; }
        configuredDeviceType = typeName;
        configuredSetup = setup;
        configuredInputChannels = inChannels;
        configuredOutputChannels = outChannels;
        routeBaseKey = typeName + "|" + inputName + "|" + outputName;
        prepareProcessorsForCurrentDevice();
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

    void restartAudio() {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (!configured.load()) { error("configure an audio device before restarting"); return; }
        if (calibrationBusy.load()) { error("wait for calibration finalization before restarting audio"); return; }

        const auto wasRunning = running.exchange(false);
        if (callbackRegistered.exchange(false)) manager.removeAudioCallback(this);
        cancelCalibration();
        manager.closeAudioDevice();
        deviceActive.store(false);

        manager.setCurrentAudioDeviceType(configuredDeviceType, true);
        if (manager.getCurrentAudioDeviceType() != configuredDeviceType) {
            configured.store(false);
            error("selected audio backend is unavailable during restart");
            return;
        }

        auto setup = configuredSetup;
        const auto result = manager.initialise(
            configuredInputChannels, configuredOutputChannels, nullptr, true, {}, &setup);
        if (result.isNotEmpty()) {
            configured.store(false);
            error(juce::String("audio engine restart failed: ") + result);
            return;
        }
        configuredSetup = setup;
        prepareProcessorsForCurrentDevice();
        configured.store(true);
        if (wasRunning || configured.load()) {
            callbackRegistered.store(true);
            manager.addAudioCallback(this);
            running.store(true);
            emitState("started");
        } else {
            emitState("configured");
        }
    }
    bool isRunning() const noexcept { return running.load(); }

    void setProtection(const juce::DynamicObject& command) {
        const auto hasEnabled = command.hasProperty("enabled");
        const auto hasPreset = command.hasProperty("preset");
        const auto hasRoute = command.hasProperty("route");
        const auto hasSuppression = command.hasProperty("suppression");
        const auto hasDepth = command.hasProperty("depth");
        const auto hasSensitivity = command.hasProperty("sensitivity");
        const auto hasTiming = command.hasProperty("timing");
        const auto hasLatch = command.hasProperty("latch");
        const auto route = static_cast<int>(getPropertyOr(command, "route", -1));
        if (hasSuppression || hasDepth || hasSensitivity || hasTiming || hasLatch) {
            if (route < 0 || route >= routeCount) {
                error("protection route is invalid"); return;
            }
            auto& processor = processors[static_cast<std::size_t>(route)];
            if (hasDepth || hasSuppression) {
                const auto amount = static_cast<float>(static_cast<double>(
                    getPropertyOr(command, hasDepth ? "depth" : "suppression", 0.75)));
                routeDepth[static_cast<std::size_t>(route)] = amount;
                processor.setDepthAmount(amount);
            }
            if (hasSensitivity) {
                const auto amount = static_cast<float>(static_cast<double>(
                    getPropertyOr(command, "sensitivity", 0.75)));
                routeSensitivity[static_cast<std::size_t>(route)] = amount;
                processor.setSensitivityAmount(amount);
            }
            if (hasTiming) {
                const auto timing = static_cast<float>(static_cast<double>(
                    getPropertyOr(command, "timing", 0.5)));
                routeTiming[static_cast<std::size_t>(route)] = timing;
                processor.setTimingAmount(timing);
            }
            if (hasLatch) {
                const auto latch = static_cast<float>(static_cast<double>(
                    getPropertyOr(command, "latch", 0.5)));
                routeLatch[static_cast<std::size_t>(route)] = latch;
                processor.setLatchAmount(latch);
            }
        }
        if (!hasEnabled && !hasPreset) {
            emitState("protection_changed");
            return;
        }
        const auto shouldEnable = static_cast<bool>(
            getPropertyOr(command, "enabled", protectionEnabled.load()));
        const auto presetName = getPropertyOr(command, "preset",
            protectionPreset.load() == werfeed::ProtectionPreset::music ? "music" : "speech").toString();
        if (presetName != "speech" && presetName != "music") {
            error("protection preset must be speech or music"); return;
        }
        const auto selected = presetName == "music"
            ? werfeed::ProtectionPreset::music : werfeed::ProtectionPreset::speech;
        if (hasRoute) {
            if (route < 0 || route >= routeCount) {
                error("protection route is invalid"); return;
            }
            processors[static_cast<std::size_t>(route)].setPreset(selected);
            processors[static_cast<std::size_t>(route)].setEnabled(shouldEnable);
        } else {
            for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex) {
                processors[static_cast<std::size_t>(routeIndex)].setPreset(selected);
                processors[static_cast<std::size_t>(routeIndex)].setEnabled(shouldEnable);
            }
        }
        protectionEnabled.store(shouldEnable);
        protectionPreset.store(selected);
        emitState("protection_changed");
    }

    void setManualNotch(const juce::DynamicObject& command) {
        const auto route = static_cast<int>(getPropertyOr(command, "route", -1));
        const auto frequency = static_cast<float>(static_cast<double>(
            getPropertyOr(command, "frequency", 0.0)));
        if (route < 0 || route >= routeCount) {
            error("manual notch route is invalid"); return;
        }
        if (!std::isfinite(frequency) || frequency < 40.0f || frequency > 20000.0f) {
            error("manual notch frequency is invalid"); return;
        }
        processors[static_cast<std::size_t>(route)].setManualNotch(frequency);
        emitState("manual_notch_added");
    }

    void clearManualNotch(const juce::DynamicObject& command) {
        const auto route = static_cast<int>(getPropertyOr(command, "route", -1));
        const auto frequency = static_cast<float>(static_cast<double>(
            getPropertyOr(command, "frequency", 0.0)));
        if (route < 0 || route >= routeCount) {
            error("manual notch route is invalid"); return;
        }
        processors[static_cast<std::size_t>(route)].clearManualNotch(frequency);
        emitState("manual_notch_removed");
    }

    void setRouteArming(const juce::DynamicObject& command) {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (!configured.load()) { error("configure an audio device before arming routes"); return; }
        const auto routeIndex = static_cast<int>(getPropertyOr(command, "route", -1));
        if (routeIndex < 0 || routeIndex >= routeCount) {
            error("route arming index is invalid"); return;
        }
        const auto enabled = static_cast<bool>(getPropertyOr(command, "enabled", false));
        const auto route = routes[static_cast<std::size_t>(routeIndex)];
        if (enabled && (route.input < 0 || route.output < 0)) {
            error("armed route must have a mapped mono input and output"); return;
        }

        const std::lock_guard<std::mutex> processorGuard(processorMutex);
        routeEnabled[static_cast<std::size_t>(routeIndex)].store(enabled, std::memory_order_release);
        for (int index = 0; index < routeCount; ++index) {
            auto& processor = processors[static_cast<std::size_t>(index)];
            processor.setNotchCapacity(notchCapacityForRoute(index));
            processor.requestNotchReset();
            processor.setEnabled(
                routeEnabled[static_cast<std::size_t>(index)].load(std::memory_order_relaxed)
                && protectionEnabled.load(std::memory_order_relaxed));
        }

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "route_arming");
        event->setProperty("route", routeIndex + 1);
        event->setProperty("enabled", enabled);
        event->setProperty("maximumAllowedNotches",
            static_cast<int>(processors[static_cast<std::size_t>(routeIndex)].getNotchCapacity()));
        emit(juce::var(event));
        emitState("route_arming_changed");
    }

    void startCalibration(const juce::DynamicObject& command) {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (!running.load() || !deviceActive.load()) { error("start active audio before calibration"); return; }
        if (calibrationBusy.exchange(true)) { error("calibration is already running or finalizing"); return; }
        const auto route = static_cast<int>(getPropertyOr(command, "route", 0));
        if (route < 0 || route >= routeCount) { calibrationBusy.store(false); error("calibration route is invalid"); return; }
        const auto mappedRoute = routes[static_cast<std::size_t>(route)];
        if (mappedRoute.input < 0 || mappedRoute.output < 0) {
            calibrationBusy.store(false);
            error("calibration route must have a mapped mono input and output");
            return;
        }
        const auto level = static_cast<float>(static_cast<double>(getPropertyOr(command, "level", 0.06)));
        if (!(level > 0.0f && level <= 0.08f)) { calibrationBusy.store(false); error("calibration level must be above 0 and at most 0.08"); return; }
        const auto rate = sampleRate.load();
        if (rate < 8000.0) { calibrationBusy.store(false); error("audio device sample rate is unavailable"); return; }
        const auto announcementPath = getPropertyOr(command, "announcementPath", "").toString();
        // Keep the spoken safety announcement clearly audible without
        // increasing the impulse/sweep level used by the measurement.
        if (!loadCalibrationAnnouncement(announcementPath, rate,
                std::min(0.12f, level * 1.5f))) {
            calibrationBusy.store(false);
            return;
        }
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
        calibrationAnnouncementGap = calibrationAnnouncement.empty()
            ? 0 : static_cast<std::size_t>(rate);
        calibrationTimelineLength = calibrationAnnouncement.size() + calibrationAnnouncementGap
            + calibrationRecording.size();
        calibrationRoute = route;
        calibrationRouteKey = keyForRoute(route);
        calibrationGeneration = generationCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        calibrationPosition.store(0);
        calibrationComplete.store(false);
        calibrating.store(true, std::memory_order_release);
        emitState("calibrating");
    }

    void resetCalibration(const juce::DynamicObject& command) {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (calibrationBusy.load()) { error("wait for calibration finalization before resetting a baseline"); return; }
        const auto route = static_cast<int>(getPropertyOr(command, "route", 0));
        if (route < 0 || route >= routeCount) { error("calibration route is invalid"); return; }
        const auto calibrationKey = keyForRoute(route);
        const auto previous = baselines.find(calibrationKey);
        const auto hadPrevious = previous != baselines.end();
        const auto previousBaseline = hadPrevious ? previous->second : Baseline {};
        baselines.erase(calibrationKey);
        processors[static_cast<std::size_t>(route)].clearBaseline();
        if (!saveCalibrations()) {
            if (hadPrevious) {
                baselines[calibrationKey] = previousBaseline;
                processors[static_cast<std::size_t>(route)].setCalibrationProfile(previousBaseline.responseDb);
            }
            error("calibration reset could not be persisted");
            return;
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "calibration_reset");
        o->setProperty("route", route + 1);
        o->setProperty("routeKey", calibrationKey);
        emit(juce::var(o));
        emitState("calibration_reset");
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        sampleRate.store(device->getCurrentSampleRate());
        bufferSize.store(device->getCurrentBufferSizeSamples());
        callbackTimingInitialised = false;
        callbackJitterRatio.store(0.0, std::memory_order_relaxed);
        callbackJitterMs.store(0.0, std::memory_order_relaxed);
        callbackJitterPeakMs.store(0.0, std::memory_order_relaxed);
        callbackExecutionMs.store(0.0, std::memory_order_relaxed);
        callbackExecutionPeakMs.store(0.0, std::memory_order_relaxed);
        callbackDeadlineMisses.store(0, std::memory_order_relaxed);
        driverXruns.store(0, std::memory_order_relaxed);
        deviceClockDriftPpm.store(0.0, std::memory_order_relaxed);
        deviceClockReady.store(false, std::memory_order_relaxed);
        deviceClockAgeMs.store(0.0, std::memory_order_relaxed);
        deviceClockMeasurementStarted = {};
        deviceClockFrames = 0;
        const std::lock_guard<std::mutex> processorGuard(processorMutex);
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
        const auto rate = sampleRate.load(std::memory_order_relaxed);
        const auto expectedPeriod = samples / std::max(1.0, rate);
        if (callbackTimingInitialised) {
            const auto interval = std::chrono::duration<double>(
                begun - lastCallbackAt).count();
            const auto jitterMs = std::abs(interval - expectedPeriod) * 1000.0;
            const auto jitterRatio = std::min(1.0,
                std::abs(interval - expectedPeriod) / std::max(1.0e-6, expectedPeriod));
            const auto smoothedRatio = callbackJitterRatio.load(std::memory_order_relaxed) * 0.95
                + jitterRatio * 0.05;
            callbackJitterRatio.store(smoothedRatio, std::memory_order_relaxed);
            callbackJitterMs.store(smoothedRatio * expectedPeriod * 1000.0,
                std::memory_order_relaxed);
            const auto previousPeak = callbackJitterPeakMs.load(std::memory_order_relaxed);
            if (jitterMs > previousPeak)
                callbackJitterPeakMs.store(jitterMs, std::memory_order_relaxed);
        } else {
            callbackTimingInitialised = true;
            deviceClockMeasurementStarted = begun;
        }
        lastCallbackAt = begun;
        deviceClockFrames += static_cast<unsigned long long>(std::max(0, samples));
        if (deviceClockMeasurementStarted != std::chrono::steady_clock::time_point {}) {
            const auto clockAge = std::chrono::duration<double>(
                begun - deviceClockMeasurementStarted).count();
            deviceClockAgeMs.store(clockAge * 1000.0, std::memory_order_relaxed);
            // A long-window frame-rate estimate filters callback scheduling
            // noise and measures the effective backend clock against QPC-backed
            // steady_clock. It is intentionally reported as an estimate until
            // the already-open WASAPI/ASIO native clock handles are available
            // through a backend-specific JUCE extension.
            if (clockAge >= 2.0 && rate > 0.0) {
                const auto observedRate = static_cast<double>(deviceClockFrames) / clockAge;
                const auto driftPpm = (observedRate - rate) / rate * 1.0e6;
                const auto smoothedDrift = deviceClockDriftPpm.load(
                    std::memory_order_relaxed) * 0.9 + driftPpm * 0.1;
                deviceClockDriftPpm.store(smoothedDrift, std::memory_order_relaxed);
                deviceClockReady.store(true, std::memory_order_relaxed);
            }
        }
        for (int channel = 0; channel < outs; ++channel) std::fill_n(output[channel], samples, 0.0f);
        float peakIn = 0.0f, peakOut = 0.0f;
        unsigned long long nonFiniteInput = 0, nonFiniteOutput = 0;
        std::array<int, werfeed::maxRoutes> monitoredOutputChannels {};
        int monitoredOutputCount = 0;
        const auto monitorOutputChannel = [&](int channel) noexcept {
            for (int index = 0; index < monitoredOutputCount; ++index)
                if (monitoredOutputChannels[static_cast<std::size_t>(index)] == channel) return;
            if (monitoredOutputCount < static_cast<int>(monitoredOutputChannels.size()))
                monitoredOutputChannels[static_cast<std::size_t>(monitoredOutputCount++)] = channel;
        };
        const auto calibrationActive = calibrating.load(std::memory_order_acquire);
        auto calibrationIndex = calibrationPosition.load(std::memory_order_relaxed);
        if (calibrationActive) {
            const auto calibrationRouteState = routes[static_cast<std::size_t>(calibrationRoute)];
            if (calibrationRouteState.input >= 0 && calibrationRouteState.input < ins) {
                const auto* source = input[calibrationRouteState.input];
                for (int frame = 0; frame < samples; ++frame) {
                    const auto value = source[frame];
                    if (!std::isfinite(value)) ++nonFiniteInput;
                    else peakIn = std::max(peakIn, std::abs(value));
                }
            }
            if (calibrationRouteState.output >= 0 && calibrationRouteState.output < outs)
                monitorOutputChannel(calibrationRouteState.output);
            werfeed::routeCalibrationWithAnnouncement(input, ins, output, outs, samples,
                routes[static_cast<std::size_t>(calibrationRoute)],
                calibrationExcitation, calibrationRecording, calibrationAnnouncement,
                calibrationAnnouncementGap, calibrationIndex);
        } else {
            for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex) {
                const auto route = routes[static_cast<std::size_t>(routeIndex)];
                if (!routeEnabled[static_cast<std::size_t>(routeIndex)].load(std::memory_order_acquire)
                    || route.input < 0 || route.input >= ins
                    || route.output < 0 || route.output >= outs)
                    continue;
                auto& processor = processors[static_cast<std::size_t>(routeIndex)];
                monitorOutputChannel(route.output);
                const auto* source = input[route.input];
                processor.pullPendingNotchUpdates();
                for (int frame = 0; frame < samples; ++frame) {
                    auto value = source[frame];
                    if (!std::isfinite(value)) {
                        ++nonFiniteInput;
                        value = 0.0f;
                    } else {
                        peakIn = std::max(peakIn, std::abs(value));
                    }
                    value = processor.process(value);
                    output[route.output][frame] += value;
                }
                // Analyze the source route. A real acoustic feedback loop
                // changes this signal after the shallow probe; a normal
                // program tone does not.
                processor.pushAnalysisBlock(input[route.input], samples);
            }
        }
        if (calibrationActive) {
            calibrationIndex += static_cast<std::size_t>(samples);
            calibrationPosition.store(calibrationIndex, std::memory_order_relaxed);
            if (calibrationIndex >= calibrationTimelineLength) {
                calibrating.store(false, std::memory_order_release);
                completedGeneration.store(calibrationGeneration, std::memory_order_relaxed);
                calibrationComplete.store(true, std::memory_order_release);
            }
        }
        for (int channelIndex = 0; channelIndex < monitoredOutputCount; ++channelIndex) {
            auto* destination = output[monitoredOutputChannels[
                static_cast<std::size_t>(channelIndex)]];
            for (int frame = 0; frame < samples; ++frame) {
                auto& value = destination[frame];
                if (!std::isfinite(value)) {
                    value = 0.0f;
                    ++nonFiniteOutput;
                } else {
                    peakOut = std::max(peakOut, std::abs(value));
                }
            }
        }
        nonFiniteInputSamples.fetch_add(nonFiniteInput, std::memory_order_relaxed);
        nonFiniteOutputSamples.fetch_add(nonFiniteOutput, std::memory_order_relaxed);
        inputPeak.store(peakIn, std::memory_order_relaxed); outputPeak.store(peakOut, std::memory_order_relaxed);
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count();
        const auto elapsedMs = elapsed * 1000.0;
        callbackExecutionMs.store(elapsedMs, std::memory_order_relaxed);
        const auto previousExecutionPeak = callbackExecutionPeakMs.load(std::memory_order_relaxed);
        if (elapsedMs > previousExecutionPeak)
            callbackExecutionPeakMs.store(elapsedMs, std::memory_order_relaxed);
        const auto budget = samples / sampleRate.load();
        cpu.store(budget > 0.0 ? elapsed / budget : 0.0, std::memory_order_relaxed);
        if (elapsed > budget) callbackDeadlineMisses.fetch_add(1, std::memory_order_relaxed);
    }

    void emitState(const char* phase) {
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "state");
        o->setProperty("phase", phase);
        o->setProperty("running", running.load());
        o->setProperty("sampleRate", sampleRate.load()); o->setProperty("bufferSize", bufferSize.load());
        emit(juce::var(o));
    }

    void emitTestMarker(const juce::DynamicObject& command) {
        const auto name = getPropertyOr(command, "name", "").toString();
        if (name.isEmpty()) { error("test_marker requires a name"); return; }
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "test_marker");
        o->setProperty("name", name);
        o->setProperty("telemetrySequence", static_cast<double>(telemetrySequence.load(std::memory_order_relaxed)));
        emit(juce::var(o));
    }

    void emitTelemetry() {
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (auto* device = manager.getCurrentAudioDevice())
            driverXruns.store(static_cast<unsigned long long>(
                std::max(0, device->getXRunCount())), std::memory_order_relaxed);
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "telemetry");
        o->setProperty("telemetrySequence", static_cast<double>(telemetrySequence.fetch_add(1, std::memory_order_relaxed)));
        o->setProperty("running", running.load());
        o->setProperty("sampleRate", sampleRate.load()); o->setProperty("bufferSize", bufferSize.load());
        o->setProperty("callbackCpu", cpu.load());
        o->setProperty("xruns", static_cast<double>(
            callbackDeadlineMisses.load(std::memory_order_relaxed)));
        o->setProperty("callbackDeadlineMisses", static_cast<double>(
            callbackDeadlineMisses.load(std::memory_order_relaxed)));
        o->setProperty("driverXruns", static_cast<double>(
            driverXruns.load(std::memory_order_relaxed)));
        o->setProperty("callbackExecutionMs", callbackExecutionMs.load());
        o->setProperty("callbackExecutionPeakMs", callbackExecutionPeakMs.load());
        o->setProperty("callbackJitterMs", callbackJitterMs.load());
        o->setProperty("callbackJitterPeakMs", callbackJitterPeakMs.load());
        o->setProperty("deviceClockDriftPpm", deviceClockDriftPpm.load());
        o->setProperty("deviceClockReady", deviceClockReady.load());
        o->setProperty("deviceClockAgeMs", deviceClockAgeMs.load());
        o->setProperty("clockMeasurementSource", clockMeasurementSource());
        o->setProperty("nonFiniteInputSamples", static_cast<double>(nonFiniteInputSamples.load()));
        o->setProperty("nonFiniteOutputSamples", static_cast<double>(nonFiniteOutputSamples.load()));
        o->setProperty("inputPeak", inputPeak.load()); o->setProperty("outputPeak", outputPeak.load());
        o->setProperty("protectionEnabled", protectionEnabled.load());
        o->setProperty("preset", protectionPreset.load() == werfeed::ProtectionPreset::music ? "music" : "speech");
        o->setProperty("calibrating", calibrating.load());
        juce::Array<juce::var> calibratedRoutes;
        for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex)
            calibratedRoutes.add(baselines.contains(keyForRoute(routeIndex)));
        o->setProperty("calibratedRoutes", juce::var(calibratedRoutes));
        o->setProperty("calibrated", routeCount > 0 && baselines.contains(keyForRoute(0)));
        juce::Array<juce::var> routeTelemetry;
        juce::Array<juce::var> spectrum;
        juce::Array<juce::var> notches;
        int activeNotches = 0;
        float maximumCut = 0.0f;
        for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex) {
            const auto snapshot = processors[static_cast<std::size_t>(routeIndex)].snapshot();
            auto* routeObject = new juce::DynamicObject();
            routeObject->setProperty("route", routeIndex + 1);
            routeObject->setProperty("enabled",
                routeEnabled[static_cast<std::size_t>(routeIndex)].load(std::memory_order_acquire) &&
                routes[static_cast<std::size_t>(routeIndex)].input >= 0 &&
                routes[static_cast<std::size_t>(routeIndex)].output >= 0);
             routeObject->setProperty("depth", snapshot.depthAmount);
             routeObject->setProperty("sensitivity", snapshot.sensitivityAmount);
             routeObject->setProperty("suppression", snapshot.suppressionAmount);
             routeObject->setProperty("timing", snapshot.timingAmount);
             routeObject->setProperty("latch", snapshot.latchAmount);
            routeObject->setProperty("activeNotches", snapshot.activeNotches);
            routeObject->setProperty("maximumAllowedNotches",
                static_cast<int>(processors[static_cast<std::size_t>(routeIndex)].getNotchCapacity()));
            routeObject->setProperty("maximumCutDb", snapshot.maximumCutDb);
            juce::Array<juce::var> routeSpectrum;
            for (const auto value : snapshot.spectrumDb) {
                routeSpectrum.add(value);
                if (routeIndex == 0) spectrum.add(value);
            }
            routeObject->setProperty("spectrumDb", juce::var(routeSpectrum));
            juce::Array<juce::var> routeNotches;
            for (const auto& notch : snapshot.notches) if (notch.active) {
                auto* n = new juce::DynamicObject();
                n->setProperty("frequency", notch.frequency);
                n->setProperty("depthDb", notch.depthDb);
                n->setProperty("q", notch.q);
                routeNotches.add(juce::var(n));
                if (routeIndex == 0) {
                    auto* legacyNotch = new juce::DynamicObject();
                    legacyNotch->setProperty("frequency", notch.frequency);
                    legacyNotch->setProperty("depthDb", notch.depthDb);
                    legacyNotch->setProperty("q", notch.q);
                    notches.add(juce::var(legacyNotch));
                }
            }
            routeObject->setProperty("notches", juce::var(routeNotches));
            const auto found = baselines.find(keyForRoute(routeIndex));
            routeObject->setProperty("calibrated", found != baselines.end());
            if (found != baselines.end()) {
                routeObject->setProperty("delayMs",
                    found->second.delaySamples * 1000.0 / std::max(1.0, sampleRate.load()));
                juce::Array<juce::var> response;
                for (const auto value : found->second.responseDb) response.add(value);
                routeObject->setProperty("calibrationResponseDb", juce::var(response));
            }
            routeTelemetry.add(juce::var(routeObject));
            if (routeIndex == 0) {
                activeNotches = snapshot.activeNotches;
                maximumCut = snapshot.maximumCutDb;
            }
        }
        o->setProperty("routeTelemetry", juce::var(routeTelemetry));
        o->setProperty("spectrumDb", juce::var(spectrum));
        o->setProperty("notches", juce::var(notches));
        o->setProperty("activeNotches", activeNotches);
        o->setProperty("maximumCutDb", maximumCut);
        o->setProperty("maximumAllowedNotches", static_cast<int>(werfeed::maxNotches));
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
        processors[static_cast<std::size_t>(calibrationRoute)].setCalibrationProfile(response);
        if (!saveCalibrations()) error("calibration completed but its baseline could not be persisted");
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "calibration");
        o->setProperty("route", calibrationRoute + 1);
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

    bool pumpAnalysis() {
        if (!running.load(std::memory_order_relaxed)) return false;
        const std::lock_guard<std::mutex> controlGuard(controlMutex);
        if (!running.load(std::memory_order_relaxed)) return false;
        const std::lock_guard<std::mutex> processorGuard(processorMutex);
        bool any = false;
        for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex)
            any = processors[static_cast<std::size_t>(routeIndex)].pumpBackgroundAnalysis() || any;
        return any;
    }

private:
    using Baseline = werfeed::CalibrationBaseline;
    void prepareProcessorsForCurrentDevice() {
        auto* device = manager.getCurrentAudioDevice();
        if (device == nullptr) return;
        const std::lock_guard<std::mutex> processorGuard(processorMutex);
        for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex) {
            auto& processor = processors[static_cast<std::size_t>(routeIndex)];
            processor.prepare(device->getCurrentSampleRate());
            processor.setNotchCapacity(notchCapacityForRoute(routeIndex));
            processor.setDepthAmount(routeDepth[static_cast<std::size_t>(routeIndex)]);
            processor.setSensitivityAmount(routeSensitivity[static_cast<std::size_t>(routeIndex)]);
            processor.setTimingAmount(routeTiming[static_cast<std::size_t>(routeIndex)]);
            processor.setLatchAmount(routeLatch[static_cast<std::size_t>(routeIndex)]);
            processor.setPreset(protectionPreset.load(std::memory_order_relaxed));
            processor.setEnabled(protectionEnabled.load(std::memory_order_relaxed));
            processor.clearBaseline();
            const auto found = baselines.find(keyForRoute(routeIndex));
            if (found != baselines.end()) processor.setCalibrationProfile(found->second.responseDb);
        }
    }
    int activeRouteCount() const noexcept {
        int count = 0;
        for (int routeIndex = 0; routeIndex < routeCount; ++routeIndex) {
            const auto route = routes[static_cast<std::size_t>(routeIndex)];
            if (routeEnabled[static_cast<std::size_t>(routeIndex)].load(std::memory_order_acquire)
                && route.input >= 0 && route.output >= 0)
                ++count;
        }
        return count;
    }
    std::size_t notchCapacityForRoute(int routeIndex) const noexcept {
        const auto route = routes[static_cast<std::size_t>(routeIndex)];
        if (!routeEnabled[static_cast<std::size_t>(routeIndex)].load(std::memory_order_acquire)
            || route.input < 0 || route.output < 0)
            return 0;
        const auto activeCount = activeRouteCount();
        if (activeCount <= 0) return 0;
        int activeOrdinal = 0;
        for (int index = 0; index < routeIndex; ++index) {
            const auto previous = routes[static_cast<std::size_t>(index)];
            if (routeEnabled[static_cast<std::size_t>(index)].load(std::memory_order_acquire)
                && previous.input >= 0 && previous.output >= 0)
                ++activeOrdinal;
        }
        return werfeed::sharedNotchCapacity(
            static_cast<std::size_t>(routeCount) * werfeed::defaultNotchesPerRoute,
            static_cast<std::size_t>(activeCount),
            static_cast<std::size_t>(activeOrdinal));
    }
    void loadCalibrations() {
        werfeed::loadCalibrationBaselines(calibrationFile, baselines);
    }
    bool saveCalibrations() {
        return werfeed::saveCalibrationBaselines(calibrationFile, baselines);
    }
    bool loadCalibrationAnnouncement(const juce::String& path, double targetRate, float level) {
        calibrationAnnouncement.clear();
        if (path.isEmpty()) return true;
        const juce::File file(path);
        if (!file.existsAsFile()) {
            error("calibration announcement file was not found");
            return false;
        }
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (!reader || reader->lengthInSamples <= 0 || reader->numChannels == 0) {
            error("calibration announcement could not be decoded");
            return false;
        }
        const auto maxSourceSamples = static_cast<juce::int64>(reader->sampleRate * 120.0);
        if (reader->lengthInSamples > maxSourceSamples) {
            error("calibration announcement is longer than 120 seconds");
            return false;
        }
        const auto sourceSamples = static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> decoded(static_cast<int>(reader->numChannels), sourceSamples);
        if (!reader->read(&decoded, 0, sourceSamples, 0, true, true)) {
            error("calibration announcement could not be read");
            return false;
        }
        const auto outputSamples = static_cast<std::size_t>(std::ceil(
            static_cast<double>(sourceSamples) * targetRate / reader->sampleRate));
        calibrationAnnouncement.resize(outputSamples);
        float peak = 0.0f;
        for (std::size_t outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
            const auto sourcePosition = static_cast<double>(outputIndex) * reader->sampleRate / targetRate;
            const auto left = std::min(sourceSamples - 1, static_cast<int>(sourcePosition));
            const auto right = std::min(sourceSamples - 1, left + 1);
            const auto fraction = static_cast<float>(sourcePosition - left);
            float sample = 0.0f;
            for (int channel = 0; channel < decoded.getNumChannels(); ++channel) {
                const auto interpolated = decoded.getSample(channel, left) * (1.0f - fraction)
                    + decoded.getSample(channel, right) * fraction;
                sample += interpolated;
            }
            sample /= static_cast<float>(decoded.getNumChannels());
            calibrationAnnouncement[outputIndex] = sample;
            peak = std::max(peak, std::abs(sample));
        }
        if (peak > level && level > 0.0f) {
            const auto scale = level / peak;
            for (auto& sample : calibrationAnnouncement) sample *= scale;
        }
        return true;
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
        const auto eligibility = werfeed::classifyDeviceTransport(
            type.getTypeName().toStdString(), name.toStdString());
        auto* d = new juce::DynamicObject();
        auto device = std::unique_ptr<juce::AudioIODevice>(
            type.createDevice(input ? name : juce::String(), input ? juce::String() : name));
        const auto channelNames = device ? (input ? device->getInputChannelNames()
                                                   : device->getOutputChannelNames()) : juce::StringArray {};
        const auto activeChannels = device
            ? (input ? device->getActiveInputChannels() : device->getActiveOutputChannels())
            : juce::BigInteger {};
        // Keep discovery device-name-first, as in the original working
        // implementation. Some WASAPI and ASIO drivers do not expose channel
        // metadata until their endpoint is opened. Dropping those records here
        // makes the renderer selectors permanently empty, so expose a mono
        // Channel 1 fallback and let configure() perform the authoritative open.
        const auto reportedChannelCount = std::max(
            channelNames.size(), activeChannels.getHighestBit() + 1);
        const auto channelCount = juce::jlimit(
            1, 64, reportedChannelCount > 0 ? reportedChannelCount : 1);
        d->setProperty("deviceType", type.getTypeName());
        d->setProperty("name", name);
        d->setProperty("interfaceName", interfaceNameFor(name));
        d->setProperty("direction", input ? "input" : "output");
        d->setProperty("transport", werfeed::deviceTransportLabel(eligibility.transport));
        d->setProperty("hardwareEligible", true);
        d->setProperty("channelMetadataReported", reportedChannelCount > 0);
        d->setProperty("channels", channelCount);
        juce::Array<juce::var> channelLabels;
        for (int channel = 0; channel < channelCount; ++channel) {
            const auto reportedName = channel < channelNames.size() ? channelNames[channel].trim() : juce::String();
            // These labels match JUCE's WASAPI channel naming convention and
            // identify the exact native channel index when a driver omits
            // human-readable metadata.
            channelLabels.add(reportedName.isNotEmpty()
                ? reportedName
                : (input ? "Input channel " : "Output channel ") + juce::String(channel + 1));
        }
        d->setProperty("channelNames", juce::var(channelLabels));
        devices.add(juce::var(d));
    }
    juce::String interfaceNameFor(const juce::String& rawName) const {
        const auto name = rawName.trim();
        const auto open = name.indexOfChar('(');
        const auto close = name.lastIndexOfChar(')');
        if (open > 0 && close > open) {
            const auto endpoint = name.substring(0, open).trim().toLowerCase();
            if (endpoint.startsWith("microphone") || endpoint.startsWith("speakers")
                || endpoint.startsWith("speaker") || endpoint.startsWith("line in")
                || endpoint.startsWith("line out") || endpoint.startsWith("headphones")
                || endpoint.startsWith("digital audio") || endpoint.startsWith("input")
                || endpoint.startsWith("output")) {
                return name.substring(open + 1, close).trim();
            }
        }
        return name;
    }
    bool parseRoutes(const juce::var& value, int inputChannels, int outputChannels) {
        auto* array = value.getArray();
        if (array == nullptr || array->size() > static_cast<int>(werfeed::maxRoutes)) { error("routes must be an array of at most 8 pairs"); return false; }
        for (int i = 0; i < array->size(); ++i) {
            auto* r = array->getReference(i).getDynamicObject();
            if (!r) { error("each route must be an object"); return false; }
            const auto enabled = static_cast<bool>(getPropertyOr(*r, "enabled", true));
            const auto inputChannel = static_cast<int>(getPropertyOr(*r, "input", -1));
            const auto outputChannel = static_cast<int>(getPropertyOr(*r, "output", -1));
            if (enabled && (inputChannel < 0 || inputChannel >= inputChannels ||
                            outputChannel < 0 || outputChannel >= outputChannels)) {
                error("enabled route channel is outside the configured mono channel range");
                return false;
            }
            routes[static_cast<size_t>(i)] = {
                inputChannel,
                outputChannel
            };
            routeEnabled[static_cast<size_t>(i)].store(enabled, std::memory_order_release);
            routeDepth[static_cast<size_t>(i)] = static_cast<float>(static_cast<double>(
                getPropertyOr(*r, "depth", getPropertyOr(*r, "suppression", 0.75))));
            routeSensitivity[static_cast<size_t>(i)] = static_cast<float>(static_cast<double>(
                getPropertyOr(*r, "sensitivity", 0.75)));
            routeTiming[static_cast<size_t>(i)] = static_cast<float>(static_cast<double>(
                getPropertyOr(*r, "timing", 0.5)));
            routeLatch[static_cast<size_t>(i)] = static_cast<float>(static_cast<double>(
                getPropertyOr(*r, "latch", 0.5)));
        }
        routeCount = array->size(); return true;
    }
    juce::AudioDeviceManager manager;
    std::array<werfeed::Route, werfeed::maxRoutes> routes {};
    std::array<std::atomic_bool, werfeed::maxRoutes> routeEnabled {};
    std::array<float, werfeed::maxRoutes> routeDepth {};
    std::array<float, werfeed::maxRoutes> routeSensitivity {};
    std::array<float, werfeed::maxRoutes> routeTiming {};
    std::array<float, werfeed::maxRoutes> routeLatch {};
    std::array<werfeed::FeedbackProcessor, werfeed::maxRoutes> processors {};
    int routeCount = 0; // only changed while callback is detached
    juce::String configuredDeviceType;
    juce::AudioDeviceManager::AudioDeviceSetup configuredSetup;
    int configuredInputChannels = 0, configuredOutputChannels = 0;
    std::atomic_bool configured { false }, running { false };
    std::atomic<double> sampleRate { 0.0 }, cpu { 0.0 };
    std::atomic<double> callbackJitterRatio { 0.0 }, callbackJitterMs { 0.0 };
    std::atomic<double> callbackJitterPeakMs { 0.0 };
    std::atomic<double> callbackExecutionMs { 0.0 }, callbackExecutionPeakMs { 0.0 };
    std::atomic<double> deviceClockDriftPpm { 0.0 }, deviceClockAgeMs { 0.0 };
    std::atomic<int> bufferSize { 0 };
    std::atomic<unsigned long long> callbackDeadlineMisses { 0 }, driverXruns { 0 };
    std::atomic<unsigned long long> nonFiniteInputSamples { 0 }, nonFiniteOutputSamples { 0 };
    std::atomic_bool deviceClockReady { false };
    std::atomic<unsigned long long> telemetrySequence { 0 };
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
    std::atomic_bool protectionEnabled { false }, calibrating { false }, calibrationComplete { false };
    std::atomic_bool calibrationBusy { false }, deviceActive { false }, deviceStopPending { false };
    std::atomic_bool callbackRegistered { false };
    std::atomic<werfeed::ProtectionPreset> protectionPreset { werfeed::ProtectionPreset::speech };
    std::atomic<std::size_t> calibrationPosition { 0 };
    std::atomic<unsigned long long> generationCounter { 0 }, completedGeneration { 0 };
    unsigned long long calibrationGeneration = 0;
    int calibrationRoute = 0;
    std::vector<float> calibrationExcitation, calibrationRecording, calibrationAnnouncement;
    std::size_t calibrationAnnouncementGap = 0, calibrationTimelineLength = 0;
    juce::File calibrationFile;
    juce::String routeBaseKey;
    juce::String calibrationRouteKey;
    std::map<juce::String, Baseline> baselines;
    std::mutex controlMutex;
    std::mutex processorMutex;
    std::chrono::steady_clock::time_point lastCallbackAt {};
    std::chrono::steady_clock::time_point deviceClockMeasurementStarted {};
    unsigned long long deviceClockFrames = 0;
    bool callbackTimingInitialised = false;

    juce::String clockMeasurementSource() const {
        const auto backend = configuredDeviceType.toLowerCase();
        if (backend.contains("asio")) return "ASIO callback-frame estimate";
        if (backend.contains("windows audio")) return "WASAPI callback-frame estimate";
        if (backend.contains("directsound")) return "DirectSound callback-frame estimate";
        return "backend callback-frame estimate";
    }
};

} // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceRuntime;
    StderrLogger logger;
    juce::Logger::setCurrentLogger(&logger);
    Engine engine;
    {
        auto* hello = new juce::DynamicObject();
        hello->setProperty("type", "hello");
        hello->setProperty("protocolVersion", 1);
        hello->setProperty("engineVersion", "0.1.7");
        emit(juce::var(hello));
    }
    std::atomic_bool done { false };
    std::thread reporter([&] { while (!done.load()) { engine.reportLifecycle(); engine.finishCalibrationIfReady(); if (engine.isRunning()) engine.emitTelemetry(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); } });
    std::thread analysisThread([&] {
        while (!done.load()) {
            if (!engine.pumpAnalysis())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
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
        else if (name == "set_route_arming") engine.setRouteArming(*object);
        else if (name == "restart_audio") engine.restartAudio();
        else if (name == "set_protection") engine.setProtection(*object);
        else if (name == "set_manual_notch") engine.setManualNotch(*object);
        else if (name == "clear_manual_notch") engine.clearManualNotch(*object);
        else if (name == "start_calibration") engine.startCalibration(*object);
        else if (name == "reset_calibration") engine.resetCalibration(*object);
        else if (name == "test_marker") engine.emitTestMarker(*object);
        else error("unknown command");
    }
    engine.stop(); done.store(true); reporter.join(); analysisThread.join();
    juce::Logger::setCurrentLogger(nullptr);
    return 0;
}