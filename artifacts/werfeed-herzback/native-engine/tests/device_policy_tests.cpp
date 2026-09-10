#include "DevicePolicy.h"

#include <cassert>

int main() {
    using werfeed::DeviceTransport;
    using werfeed::classifyDeviceTransport;

    // Manufacturer/backend naming varies across Windows drivers. Keep the
    // accepted transport families explicit so a live enumeration can be
    // checked against the same policy as the native engine.
    assert(classifyDeviceTransport("Windows Audio (Exclusive Mode)",
        "Speakers (USB Audio Device)").transport == DeviceTransport::usb);
    assert(classifyDeviceTransport("ASIO", "Focusrite USB ASIO").allowed);
    assert(classifyDeviceTransport("ASIO", "Dante Virtual Soundcard").transport == DeviceTransport::ethernet);
    assert(classifyDeviceTransport("ASIO", "Waves SoundGrid ASIO").transport == DeviceTransport::ethernet);
    assert(classifyDeviceTransport("Windows Audio", "AES67 network input").transport == DeviceTransport::ethernet);
    assert(classifyDeviceTransport("ASIO", "RAVENNA ASIO").transport == DeviceTransport::ethernet);
    assert(classifyDeviceTransport("ASIO", "AVB Network Audio").transport == DeviceTransport::ethernet);

    assert(!classifyDeviceTransport("Windows Audio", "Speakers (Realtek High Definition Audio)").allowed);
    assert(!classifyDeviceTransport("Windows Audio", "USB Realtek High Definition Audio").allowed);
    assert(!classifyDeviceTransport("Windows Audio", "Headphones (Bluetooth)").allowed);
    assert(!classifyDeviceTransport("DirectSound", "NVIDIA HDMI Output").allowed);
    assert(!classifyDeviceTransport("Windows Audio", "DisplayPort Audio").allowed);
    assert(!classifyDeviceTransport("Windows Audio", "Speakers (USB Virtual Audio)").allowed);
    assert(!classifyDeviceTransport("Windows Audio", "USB Bluetooth Headset").allowed);
    assert(!classifyDeviceTransport("ASIO", "ASIO4ALL v2").allowed);
    assert(!classifyDeviceTransport("Virtual Audio", "Virtual Cable Output").allowed);
}