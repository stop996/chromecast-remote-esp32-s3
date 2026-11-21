#pragma once

#include <stdint.h>

namespace usbMic {

// Two steps per unit so 1.5x can be stored without a floating-point value.
constexpr uint8_t unityGainSteps = 2;
constexpr uint8_t gainOptions[] = {2, 3, 4, 6, 8, 12, 16, 24, 32};

inline bool validGainSteps(unsigned long steps) {
    for (uint8_t option : gainOptions) {
        if (steps == option) return true;
    }
    return false;
}

constexpr int16_t clampPcm(int32_t value) {
    return value > 32767 ? 32767 : value < -32768 ? -32768 : static_cast<int16_t>(value);
}

constexpr int16_t applyGain(int16_t sample, int32_t hostGain, uint8_t steps) {
    return clampPcm(static_cast<int32_t>((int64_t(sample) * hostGain * steps) / 65536));
}

}  // namespace usbMic
