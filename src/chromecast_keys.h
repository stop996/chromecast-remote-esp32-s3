#pragma once

#include <stddef.h>
#include <stdint.h>

namespace chromecast {

constexpr uint8_t reportId = 1;

struct Key {
    uint16_t usage;
    const char* name;
    const char* shortcut;
};

// The HID array contains selectors (1-based indexes into the usage list).
constexpr Key keys[] = {
    {0x019e, "POWER", "Esc"},
    {0x00cd, "PLAY_PAUSE", ""},
    {0x0042, "UP", "Ctrl+["},
    {0x0043, "DOWN", "Ctrl+]"},
    {0x0044, "LEFT", "Left"},
    {0x0045, "RIGHT", "Right"},
    {0x0041, "OK", "Enter"},
    {0x00e2, "MUTE", "Ctrl+Alt+Shift+N"},
    {0x0221, "ASSISTANT", "RAlt+RCtrl"},
    {0x0223, "HOME", "Ctrl+Alt+Shift+T"},
    {0x0224, "BACK", "Backspace"},
    {0x00e9, "VOLUME_UP", "VolumeUp"},
    {0x00ea, "VOLUME_DOWN", "VolumeDown"},
    {0x0077, "YOUTUBE", ""},
    {0x0078, "NETFLIX", "Ctrl+Alt+Shift+N"},
    {0x0079, "BRIGHTNESS_UP", ""},
    {0x0089, "INPUT", ""},
};
constexpr size_t keyCount = sizeof(keys) / sizeof(keys[0]);
constexpr size_t assistantIndex = 8;
static_assert(keys[assistantIndex].usage == 0x0221, "Assistant usage must match the ATVV request mapping");
constexpr uint8_t standardSlots = 2;
constexpr uint8_t extendedSlots = 8;

// Signature of the Consumer Control map captured on 18D1:9450.
// Build the usage items from the table so the map and labels cannot drift apart.
inline uint8_t selectorCount(const uint8_t* bytes, size_t size) {
    constexpr uint8_t header[] = {0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, reportId};
    constexpr uint8_t range[] = {0x15, 0x01, 0x25, keyCount, 0x95};
    constexpr uint8_t trailer[] = {0x75, 0x08, 0x81, 0x00, 0xc0};
    size_t offset = 0;
    auto take = [&](uint8_t value) {
        return offset < size && bytes[offset++] == value;
    };
    for (uint8_t value : header) {
        if (!take(value)) return 0;
    }
    for (const Key& key : keys) {
        const uint8_t high = key.usage >> 8;
        if (!take(high ? 0x0a : 0x09) || !take(key.usage & 0xff) || (high && !take(high))) {
            return 0;
        }
    }
    for (uint8_t value : range) {
        if (!take(value)) return 0;
    }
    if (offset == size) return 0;
    const uint8_t slots = bytes[offset++];
    if (slots != standardSlots && slots != extendedSlots) return 0;
    for (uint8_t value : trailer) {
        if (!take(value)) return 0;
    }
    return offset == size ? slots : 0;
}

// HOGP carries the report ID out of band. Some Chromecast remotes pad the
// two-selector report to eight bytes. Only a matching descriptor authorizes
// eight active selectors. Other report IDs and nonzero padding stay raw.
inline bool decodeReport(const uint8_t* bytes, size_t size, int id, uint8_t knownSlots,
                         bool chromecastWithAtvv, uint32_t& pressed) {
    if (id != reportId || (size != standardSlots && size != extendedSlots)) return false;
    if (!knownSlots && !(chromecastWithAtvv && size == extendedSlots)) return false;
    const size_t slots = knownSlots == extendedSlots && size == extendedSlots ? extendedSlots : standardSlots;
    uint32_t mask = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t selector = bytes[i];
        if (selector > (i < slots ? keyCount : 0)) return false;
        if (selector) mask |= uint32_t{1} << (selector - 1);
    }
    pressed = mask;
    return true;
}

template <typename Emit>
void changes(uint32_t before, uint32_t after, Emit emit) {
    const uint32_t released = before & ~after;
    const uint32_t pressed = after & ~before;
    for (size_t i = 0; i < keyCount; ++i) {
        if (released & (uint32_t{1} << i)) emit(keys[i], false);
    }
    for (size_t i = 0; i < keyCount; ++i) {
        if (pressed & (uint32_t{1} << i)) emit(keys[i], true);
    }
}

}  // namespace chromecast
