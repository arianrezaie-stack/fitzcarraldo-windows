#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct ExpectedRoute {
    int route;
    bool enabled;
    int capacity;
};

struct ExpectedStep {
    std::string_view name;
    std::array<ExpectedRoute, 4> routes;
};

constexpr std::array<ExpectedStep, 7> expectedSteps {{
    { "notch-four-active", {{
        { 1, true, 8 }, { 2, true, 8 }, { 3, true, 8 }, { 4, true, 8 },
    }}},
    { "notch-three-active", {{
        { 1, true, 11 }, { 2, true, 11 }, { 3, true, 10 }, { 4, false, 0 },
    }}},
    { "notch-two-active", {{
        { 1, true, 16 }, { 2, true, 16 }, { 3, false, 0 }, { 4, false, 0 },
    }}},
    { "notch-one-active", {{
        { 1, true, 32 }, { 2, false, 0 }, { 3, false, 0 }, { 4, false, 0 },
    }}},
    { "notch-two-active-restored", {{
        { 1, true, 16 }, { 2, true, 16 }, { 3, false, 0 }, { 4, false, 0 },
    }}},
    { "notch-three-active-restored", {{
        { 1, true, 11 }, { 2, true, 11 }, { 3, true, 10 }, { 4, false, 0 },
    }}},
    { "notch-four-active-restored", {{
        { 1, true, 8 }, { 2, true, 8 }, { 3, true, 8 }, { 4, true, 8 },
    }}},
}};

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "notch redistribution fixture failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

juce::DynamicObject* objectFrom(const juce::var& value, const std::string& context) {
    if (auto* object = value.getDynamicObject())
        return object;
    fail(context + " is not a JSON object");
}

juce::var requiredProperty(const juce::DynamicObject& object,
                           const char* property,
                           const std::string& context) {
    const juce::Identifier identifier(property);
    if (!object.hasProperty(identifier))
        fail(context + " is missing " + property);
    return object.getProperty(identifier);
}

double numberProperty(const juce::DynamicObject& object,
                      const char* property,
                      const std::string& context) {
    const auto value = requiredProperty(object, property, context);
    if (!value.isInt() && !value.isInt64() && !value.isDouble())
        fail(context + " has non-numeric " + property);
    const auto number = static_cast<double>(value);
    if (!std::isfinite(number))
        fail(context + " has non-finite " + property);
    return number;
}

bool booleanProperty(const juce::DynamicObject& object,
                     const char* property,
                     const std::string& context) {
    const auto value = requiredProperty(object, property, context);
    if (!value.isBool())
        fail(context + " has non-boolean " + property);
    return static_cast<bool>(value);
}

const ExpectedStep* findStep(std::string_view name) {
    for (const auto& step : expectedSteps)
        if (step.name == name)
            return &step;
    return nullptr;
}

void validateTelemetry(const juce::DynamicObject& telemetry,
                       const ExpectedStep& expected) {
    const auto context = std::string("step ") + std::string(expected.name);
    const auto callbackCpu = numberProperty(telemetry, "callbackCpu", context);
    if (!booleanProperty(telemetry, "running", context) ||
        callbackCpu < 0.0 || callbackCpu > 1.0)
        fail(context + " is not running or has unsafe callback CPU");

    if (numberProperty(telemetry, "xruns", context) != 0.0)
        fail(context + " reports an xrun");
    const auto clockStability = numberProperty(telemetry, "clockStability", context);
    if (clockStability < 0.0 || clockStability > 1.0)
        fail(context + " has invalid clock stability");
    for (const auto* peak : { "inputPeak", "outputPeak" }) {
        const auto value = numberProperty(telemetry, peak, context);
        if (value < 0.0 || value > 2.0)
            fail(context + " has invalid " + peak);
    }

    const auto routeValue = requiredProperty(telemetry, "routeTelemetry", context);
    if (!routeValue.isArray() || routeValue.size() != 4)
        fail(context + " must contain exactly four route slots");

    std::array<bool, 4> seenRoutes {};
    for (int index = 0; index < routeValue.size(); ++index) {
        auto* route = objectFrom(routeValue[index], context + " route telemetry");
        const auto routeNumber = static_cast<int>(numberProperty(
            *route, "route", context + " route telemetry"));
        if (routeNumber < 1 || routeNumber > 4 || seenRoutes[routeNumber - 1])
            fail(context + " has missing or duplicate route indices");
        seenRoutes[routeNumber - 1] = true;

        const auto& expectedRoute = expected.routes[static_cast<std::size_t>(routeNumber - 1)];
        const auto routeContext = context + " route " + std::to_string(routeNumber);
        if (booleanProperty(*route, "enabled", routeContext) != expectedRoute.enabled)
            fail(routeContext + " has the wrong enabled state");

        const auto capacity = numberProperty(*route, "maximumAllowedNotches", routeContext);
        if (capacity != expectedRoute.capacity)
            fail(routeContext + " has capacity " + std::to_string(static_cast<int>(capacity)) +
                 ", expected " + std::to_string(expectedRoute.capacity));

        const auto activeNotches = numberProperty(*route, "activeNotches", routeContext);
        if (activeNotches > capacity)
            fail(routeContext + " has active cuts above capacity");
        if (!expectedRoute.enabled && activeNotches != 0.0)
            fail(routeContext + " retained stale cuts while disarmed");
    }
    for (const auto routeSeen : seenRoutes)
        if (!routeSeen)
            fail(context + " is missing a route index");
}

} // namespace

int main() {
    std::ifstream fixture(WERFEED_NOTCH_FIXTURE_PATH);
    if (!fixture)
        fail("unable to open " WERFEED_NOTCH_FIXTURE_PATH);

    std::array<bool, expectedSteps.size()> seenSteps {};
    std::array<bool, expectedSteps.size()> telemetrySeen {};
    int currentStep = -1;
    std::string line;
    int lineNumber = 0;
    while (std::getline(fixture, line)) {
        ++lineNumber;
        if (line.empty())
            continue;
        juce::var event;
        const auto parseResult = juce::JSON::parse(juce::String(line), event);
        if (parseResult.failed())
            fail("line " + std::to_string(lineNumber) + " is invalid JSON");
        auto* object = objectFrom(event, "line " + std::to_string(lineNumber));
        const auto type = requiredProperty(
            *object, "type", "line " + std::to_string(lineNumber)).toString().toStdString();

        if (type == "error")
            fail("line " + std::to_string(lineNumber) + " contains an engine error");
        if (type == "stop" ||
            (type == "state" &&
             (object->getProperty("phase").toString() == "stopped" ||
              object->getProperty("phase").toString() == "device_stopped")))
            fail("line " + std::to_string(lineNumber) + " contains a stop event");

        if (type == "test_marker") {
            const auto marker = requiredProperty(
                *object, "name", "line " + std::to_string(lineNumber)).toString().toStdString();
            const auto* step = findStep(marker);
            if (step == nullptr)
                continue;
            currentStep = static_cast<int>(step - expectedSteps.data());
            if (seenSteps[static_cast<std::size_t>(currentStep)])
                fail("step " + marker + " appears more than once");
            seenSteps[static_cast<std::size_t>(currentStep)] = true;
            continue;
        }
        if (type == "telemetry" && currentStep >= 0) {
            if (telemetrySeen[static_cast<std::size_t>(currentStep)])
                continue;
            validateTelemetry(*objectFrom(event, "telemetry"),
                              expectedSteps[static_cast<std::size_t>(currentStep)]);
            telemetrySeen[static_cast<std::size_t>(currentStep)] = true;
        }
    }

    for (std::size_t index = 0; index < expectedSteps.size(); ++index) {
        if (!seenSteps[index])
            fail("missing marker for step " + std::string(expectedSteps[index].name));
        if (!telemetrySeen[index])
            fail("missing telemetry for step " + std::string(expectedSteps[index].name));
    }
    return EXIT_SUCCESS;
}