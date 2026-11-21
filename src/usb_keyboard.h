#pragma once
#include <stddef.h>
#include <stdint.h>

namespace usbKeyboard {
struct Status {
    bool mounted;
    bool ready;
    size_t queued;
    uint32_t down;
    uint32_t up;
};
void begin();
void trigger(const char* text);
void holdVoice(const char* text, bool down);
void cancel();
void poll();
Status status();
}
