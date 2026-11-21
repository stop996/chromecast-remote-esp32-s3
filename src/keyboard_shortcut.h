#pragma once

#include <stddef.h>
#include <stdint.h>

namespace keyboardShortcut {

constexpr size_t textSize = 64;
constexpr size_t maxKeys = 6;

struct Chord {
    uint8_t modifiers = 0;
    uint8_t keys[maxKeys]{};
    uint16_t consumer = 0;

    bool empty() const { return modifiers == 0 && keys[0] == 0 && consumer == 0; }
};

// Empty text clears the mapping. A media action uses Consumer Control on its own.
bool parse(const char* text, Chord& chord);

}  // namespace keyboardShortcut
