#pragma once

#include <Arduino.h>

#if defined(CHROMECAST_USB_MIC)

#include <USBCDC.h>

USBCDC& tty();

namespace usbMic {
bool begin();
bool listening();
void setGainSteps(uint8_t steps);
void startAudio();
void pushAudio(const int16_t* samples, size_t count);
void stopAudio(bool discard);
}

#else

inline auto tty() -> decltype((Serial)) { return Serial; }

#endif
