#pragma once

#include <stdint.h>

namespace usbMic {

constexpr uint16_t sampleRate = 16000;
constexpr uint8_t interval = 2;  // Full-speed isochronous bInterval=2 is a 2 ms poll interval.
constexpr uint8_t samplesPerPacket = (sampleRate / 1000) * (1 << (interval - 1));
constexpr uint8_t packetBytes = samplesPerPacket * sizeof(int16_t);
constexpr uint8_t featureUnit = 2;
constexpr uint8_t associationBytes = 8;
constexpr uint8_t endpointOffset = 92;

// USB Audio Class 1.0. Mono PCM signed 16-bit, 16 kHz, 32 samples every 2 ms.
struct AudioDescriptor {
    uint8_t data[108];
};

constexpr AudioDescriptor audioDescriptor(uint8_t control, uint8_t stream, uint8_t endpoint, uint8_t name) {
    return {{
        8, 0x0b, control, 2, 0x01, 0x00, 0x00, name,       // Audio interface association
        9, 0x04, control, 0, 0, 0x01, 0x01, 0x00, name,    // Audio Control interface
        9, 0x24, 0x01, 0x00, 0x01, 39, 0, 1, stream,       // UAC1, one streaming interface
        12, 0x24, 0x02, 1, 0x01, 0x02, 0, 1, 0, 0, 0, 0, // Microphone input terminal
        9, 0x24, 0x06, featureUnit, 1, 1, 0x03, 0, 0,    // Master mute and volume
        9, 0x24, 0x03, 3, 0x01, 0x01, 0, featureUnit, 0,  // USB streaming output terminal
        9, 0x04, stream, 0, 0, 0x01, 0x02, 0x00, 0,       // Streaming off
        9, 0x04, stream, 1, 1, 0x01, 0x02, 0x00, 0,       // Streaming on
        7, 0x24, 0x01, 3, 1, 0x01, 0x00,                  // Link to USB output terminal, PCM
        11, 0x24, 0x02, 1, 1, 2, 16, 1,                   // Type I, one discrete sample rate
        uint8_t(sampleRate & 0xff), uint8_t(sampleRate >> 8), 0,
        9, 0x05, endpoint, 0x05, packetBytes, 0, interval, 0, 0,  // Isochronous IN, remote audio clock
        7, 0x25, 0x01, 0, 0, 0, 0                         // Fixed clock, no endpoint controls
    }};
}

}  // namespace usbMic
