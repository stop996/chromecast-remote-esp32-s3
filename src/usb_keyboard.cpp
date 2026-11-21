#if defined(CHROMECAST_USB_MIC)

#include "usb_keyboard.h"

#include <USBHIDKeyboard.h>
#include <USBHIDConsumerControl.h>
#include <tusb.h>
#include <algorithm>

#include "keyboard_shortcut.h"

namespace usbKeyboard {
namespace {

constexpr size_t capacity = 16;
keyboardShortcut::Chord queued[capacity];
size_t head = 0;
size_t count = 0;
bool releasePending = false;
bool releaseConsumer = false;
keyboardShortcut::Chord voiceChord;
keyboardShortcut::Chord pendingVoiceChord;
bool voiceWanted = false;
bool voiceDownPending = false;
bool voiceUpPending = false;
bool voiceActive = false;
uint32_t downReports = 0;
uint32_t upReports = 0;

bool online() { return tud_mounted() && !tud_suspended(); }

bool send(keyboardShortcut::Chord chord, bool consumerRelease = false) {
    if (chord.consumer || consumerRelease) {
        const uint8_t report[] = {uint8_t(chord.consumer), uint8_t(chord.consumer >> 8)};
        return tud_hid_n_report(0, HID_REPORT_ID_CONSUMER_CONTROL, report, sizeof(report));
    }
    return tud_hid_n_keyboard_report(0, HID_REPORT_ID_KEYBOARD, chord.modifiers, chord.keys);
}

void enqueue(const keyboardShortcut::Chord& chord) {
    if (count == capacity) return;
    queued[(head + count) % capacity] = chord;
    ++count;
}

bool mergeVoice(keyboardShortcut::Chord& chord) {
    if (!voiceActive || chord.consumer) return true;
    auto combined = voiceChord;
    combined.modifiers |= chord.modifiers;
    auto* end = combined.keys + keyboardShortcut::maxKeys;
    for (uint8_t key : chord.keys) {
        if (!key || std::find(combined.keys, end, key) != end) continue;
        auto* slot = std::find(combined.keys, end, 0);
        if (slot == end) return false;
        *slot = key;
    }
    chord = combined;
    return true;
}

}  // namespace

void begin() {
    // Both reports share the Arduino HID interface and endpoint.
    static USBHIDKeyboard keyboard;
    static USBHIDConsumerControl consumer;
}

void trigger(const char* text) {
    if (!*text || !online() || count == capacity) return;
    keyboardShortcut::Chord chord;
    if (keyboardShortcut::parse(text, chord) && !chord.empty()) enqueue(chord);
}

void holdVoice(const char* text, bool down) {
    if (!down) {
        voiceWanted = false;
        voiceUpPending = voiceActive;
        return;
    }
    if (!*text || !online()) return;
    keyboardShortcut::Chord chord;
    if (!keyboardShortcut::parse(text, chord) || chord.empty()) return;
    if (chord.consumer) {
        enqueue(chord);
        return;
    }
    voiceWanted = true;
    if (!voiceActive || voiceUpPending) {
        pendingVoiceChord = chord;
        voiceDownPending = true;
    }
}

void cancel() {
    count = 0;
    voiceWanted = voiceDownPending = false;
    voiceUpPending = voiceActive;
}

void poll() {
    if (!online()) {
        cancel();
        return;
    }
    const bool releaseVoice = voiceActive && (voiceUpPending || !voiceWanted);
    if ((!releasePending && !count && !voiceDownPending && !releaseVoice) ||
        !tud_hid_n_ready(0)) return;
    if (releasePending) {
        if (send(voiceActive && !releaseConsumer ? voiceChord : keyboardShortcut::Chord{}, releaseConsumer)) {
            ++upReports;
            releasePending = false;
        }
        return;
    }
    if (releaseVoice) {
        if (send({})) {
            ++upReports;
            voiceUpPending = false;
            voiceActive = false;
        }
        return;
    }
    if (voiceDownPending) {
        if (send(pendingVoiceChord)) {
            ++downReports;
            voiceChord = pendingVoiceChord;
            voiceDownPending = false;
            voiceActive = true;
        }
        return;
    }
    auto chord = queued[head];
    if (mergeVoice(chord)) {
        if (!send(chord)) return;
        ++downReports;
        releaseConsumer = chord.consumer != 0;
        releasePending = true;
    }
    head = (head + 1) % capacity;
    --count;
}

Status status() {
    return {tud_mounted(), tud_hid_n_ready(0), count, downReports, upReports};
}

}  // namespace usbKeyboard

#endif
