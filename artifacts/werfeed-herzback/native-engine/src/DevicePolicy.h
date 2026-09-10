#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace werfeed {

enum class DeviceTransport {
    unknown,
    usb,
    ethernet,
};

struct DeviceTransportDecision {
    bool allowed = false;
    DeviceTransport transport = DeviceTransport::unknown;
};

inline std::string lowerDeviceText(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return result;
}

inline bool containsDeviceText(const std::string& value, std::string_view needle) {
    return value.find(needle) != std::string::npos;
}

inline DeviceTransportDecision classifyDeviceTransport(std::string_view deviceType,
                                                       std::string_view deviceName) {
    const auto type = lowerDeviceText(deviceType);
    const auto name = lowerDeviceText(deviceName);
    const auto combined = type + " " + name;

    constexpr std::string_view blocked[] {
        "bluetooth", "hdmi", "displayport", "display audio", "stereo mix",
        "voicemeeter", "vb-audio", "asio4all", "fl studio asio", "loopback",
        "virtual audio", "virtual cable", "blackhole", "realtek high definition",
        "high definition audio", "built-in", "built in", "onboard"
    };
    for (const auto token : blocked)
        if (containsDeviceText(combined, token))
            return {};

    constexpr std::string_view ethernet[] {
        "dante", "soundgrid", "sound grid", "aes67", "ravenna", "avb",
        "audio over ethernet"
    };
    for (const auto token : ethernet)
        if (containsDeviceText(combined, token))
            return { true, DeviceTransport::ethernet };

    constexpr std::string_view usb[] { "usb", "usb audio", "usb asio" };
    for (const auto token : usb)
        if (containsDeviceText(combined, token))
            return { true, DeviceTransport::usb };

    return {};
}

inline const char* deviceTransportLabel(DeviceTransport transport) noexcept {
    switch (transport) {
        case DeviceTransport::usb: return "USB";
        case DeviceTransport::ethernet: return "Ethernet audio";
        default: return "Unknown";
    }
}

} // namespace werfeed