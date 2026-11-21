#include "keyboard_shortcut.h"

#include <string.h>

namespace keyboardShortcut {
namespace {

struct NamedKey {
    const char* name;
    uint8_t usage;
};

constexpr NamedKey namedKeys[] = {
    {"CTRL", 0xe0}, {"SHIFT", 0xe1}, {"ALT", 0xe2}, {"WIN", 0xe3},
    {"RCTRL", 0xe4}, {"RSHIFT", 0xe5}, {"RALT", 0xe6}, {"RWIN", 0xe7},
    {"ENTER", 0x28}, {"ESC", 0x29}, {"BACKSPACE", 0x2a}, {"TAB", 0x2b},
    {"SPACE", 0x2c}, {"CAPSLOCK", 0x39}, {"PRINTSCREEN", 0x46}, {"SCROLLLOCK", 0x47},
    {"PAUSE", 0x48}, {"INSERT", 0x49}, {"HOME", 0x4a}, {"PAGEUP", 0x4b},
    {"DELETE", 0x4c}, {"END", 0x4d}, {"PAGEDOWN", 0x4e}, {"RIGHT", 0x4f},
    {"LEFT", 0x50}, {"DOWN", 0x51}, {"UP", 0x52}, {"NUMLOCK", 0x53},
    {"MENU", 0x65},
};
constexpr NamedKey consumerKeys[] = {
    {"VOLUMEUP", 0xe9}, {"VOLUMEDOWN", 0xea}, {"VOLUMEMUTE", 0xe2},
};

constexpr char punctuation[] = "-=[]\\#;'`,./";
bool digit(char c) { return c >= '0' && c <= '9'; }
bool blank(char c) { return c == ' '; }

uint8_t keyUsage(const char* token, size_t length) {
    if (length == 1) {
        const char c = token[0];
        if (c >= 'A' && c <= 'Z') return 0x04 + c - 'A';
        if (c >= '1' && c <= '9') return 0x1e + c - '1';
        if (c == '0') return 0x27;
        const char* symbol = strchr(punctuation, c);
        if (symbol) return 0x2d + (symbol - punctuation);
    }
    if ((length == 2 || length == 3) && token[0] == 'F' && token[1] >= '1' && token[1] <= '9' &&
        (length == 2 || digit(token[2]))) {
        const unsigned number = length == 2 ? token[1] - '0' : 10 * (token[1] - '0') + token[2] - '0';
        if (number <= 12) return 0x3a + number - 1;
        if (number <= 24) return 0x68 + number - 13;
    }
    for (const auto& key : namedKeys) {
        if (strcmp(token, key.name) == 0) return key.usage;
    }
    return 0;
}

}  // namespace

bool parse(const char* text, Chord& chord) {
    Chord parsed;
    while (blank(*text)) ++text;
    if (!*text) {
        chord = parsed;
        return true;
    }
    size_t count = 0;
    do {
        while (blank(*text)) ++text;
        char token[12];
        size_t length = 0;
        while (*text && *text != '+' && !blank(*text)) {
            if (length >= sizeof(token) - 1) return false;
            const char c = *text++;
            token[length++] = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
        }
        token[length] = '\0';
        if (!length) return false;
        while (blank(*text)) ++text;
        const uint8_t usage = keyUsage(token, length);
        if (!usage) {
            if (!parsed.empty() || *text) return false;
            for (const auto& key : consumerKeys) {
                if (strcmp(token, key.name) == 0) {
                    chord = {};
                    chord.consumer = key.usage;
                    return true;
                }
            }
            return false;
        }
        if (usage >= 0xe0) {
            const uint8_t bit = 1 << (usage - 0xe0);
            if (parsed.modifiers & bit) return false;
            parsed.modifiers |= bit;
        } else {
            if (count >= maxKeys) return false;
            for (size_t i = 0; i < count; ++i) {
                if (parsed.keys[i] == usage) return false;
            }
            parsed.keys[count++] = usage;
        }
        if (!*text) {
            chord = parsed;
            return true;
        }
        if (*text++ != '+') return false;
    } while (true);
}

}  // namespace keyboardShortcut
