#pragma once

#include <Preferences.h>
#include <WebServer.h>

#include "chromecast_keys.h"
#if defined(CHROMECAST_USB_MIC)
#include "keyboard_shortcut.h"
#include "usb_audio_gain.h"
#endif

class SetupPortal {
public:
    // The main loop owns all BLE state and handles the portal's requests.
    void begin(Preferences* storage, String (*remoteStatus)(),
               const char* (*remoteAction)(const String&, int), void (*beforeChange)());
    bool configured() const { return configured_; }
    bool active() const { return active_; }
    bool open();
    void poll();
    const char* eventName(size_t key) const { return mapping_.tty.names[key]; }
#if defined(CHROMECAST_USB_MIC)
    const char* shortcut(size_t key) const { return mapping_.shortcuts[key]; }
#endif
    void recordKey(const char* key, const char* action);

private:
    static constexpr size_t nameSize = 25;
    struct Names {
        uint8_t version = 1;
        char names[chromecast::keyCount][nameSize]{};
    };
    struct Mapping {
        Names tty;
#if defined(CHROMECAST_USB_MIC)
        char shortcuts[chromecast::keyCount][keyboardShortcut::textSize]{};
        uint8_t micGainSteps = usbMic::unityGainSteps;
#endif
    };
#if defined(CHROMECAST_USB_MIC)
    static constexpr size_t keyMappingBytes = offsetof(Mapping, micGainSteps);
#else
    static constexpr size_t keyMappingBytes = sizeof(Mapping);
#endif

    bool validMapping(const Mapping& mapping) const;
    void routes();
    bool requestAllowed(bool write);
    void sendJson(int status, const String& payload);
    void sendConfig();
    void sendStatus();
    void save();

    WebServer server_{80};
    Preferences* storage_ = nullptr;
    String (*remoteStatus_)() = nullptr;
    const char* (*remoteAction_)(const String&, int) = nullptr;
    void (*beforeChange_)() = nullptr;
    Mapping mapping_;
    bool configured_ = false;
    bool active_ = false;
    bool closing_ = false;
    uint32_t closeAt_ = 0;
    String address_;
    String token_;
    const char* lastKey_ = "";
    const char* lastAction_ = "";
};

void appendJsonString(String& output, const char* value);
