#include "DevicePolicy.h"

#include <iostream>

#define REQUIRE(...) \
    do { \
        if (!(__VA_ARGS__)) { \
            std::cerr << "device-policy-tests: check failed: " << #__VA_ARGS__ \
                      << " (line " << __LINE__ << ")\n"; \
            return 1; \
        } \
    } while (false)

int main() {
    using werfeed::DeviceTransport;
    using werfeed::classifyDeviceTransport;

    // Manufacturer/backend naming varies across Windows drivers. Keep the
    // accepted transport families explicit so a live enumeration can be
    // checked against the same policy as the native engine.
    REQUIRE(classifyDeviceTransport("Windows Audio (Exclusive Mode)",
        "Speakers (USB Audio Device)").transport == DeviceTransport::usb);
    REQUIRE(classifyDeviceTransport("ASIO", "Focusrite USB ASIO").allowed);
    REQUIRE(classifyDeviceTransport("ASIO", "Dante Virtual Soundcard").transport == DeviceTransport::ethernet);
    REQUIRE(classifyDeviceTransport("ASIO", "Waves SoundGrid ASIO").transport == DeviceTransport::ethernet);
    REQUIRE(classifyDeviceTransport("Windows Audio", "AES67 network input").transport == DeviceTransport::ethernet);
    REQUIRE(classifyDeviceTransport("ASIO", "RAVENNA ASIO").transport == DeviceTransport::ethernet);
    REQUIRE(classifyDeviceTransport("ASIO", "AVB Network Audio").transport == DeviceTransport::ethernet);

    REQUIRE(!classifyDeviceTransport("Windows Audio", "Speakers (Realtek High Definition Audio)").allowed);
    REQUIRE(!classifyDeviceTransport("Windows Audio", "USB Realtek High Definition Audio").allowed);
    REQUIRE(!classifyDeviceTransport("Windows Audio", "Headphones (Bluetooth)").allowed);
    REQUIRE(!classifyDeviceTransport("DirectSound", "NVIDIA HDMI Output").allowed);
    REQUIRE(!classifyDeviceTransport("Windows Audio", "DisplayPort Audio").allowed);
    REQUIRE(!classifyDeviceTransport("Windows Audio", "Speakers (USB Virtual Audio)").allowed);
    REQUIRE(!classifyDeviceTransport("Windows Audio", "USB Bluetooth Headset").allowed);
    REQUIRE(!classifyDeviceTransport("ASIO", "ASIO4ALL v2").allowed);
    REQUIRE(!classifyDeviceTransport("Virtual Audio", "Virtual Cable Output").allowed);
    return 0;
}