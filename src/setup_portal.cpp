#include "setup_portal.h"

#include <WiFi.h>

#include "setup_page.h"
#include "usb_microphone.h"

namespace {

#if defined(CHROMECAST_USB_MIC)
constexpr char storageKey[] = "keymap-gain";
constexpr char previousStorageKey[] = "keymap-hid";

const char* updatedShortcut(const char* text, const chromecast::Key& key) {
    constexpr uint8_t ctrlShift = 0x03;
    constexpr uint8_t ctrlAlt = 0x05;
    constexpr uint8_t letterN = 0x11;
    constexpr uint8_t letterT = 0x17;
    constexpr uint16_t home = 0x0223;
    keyboardShortcut::Chord chord;
    if (!keyboardShortcut::parse(text, chord) || chord.keys[1]) return nullptr;
    if (chord.keys[0] == letterN && chord.modifiers == ctrlShift) return "Ctrl+Alt+Shift+N";
    if (chord.keys[0] == letterT &&
        (chord.modifiers == ctrlShift || (key.usage == home && chord.modifiers == ctrlAlt))) {
        return "Ctrl+Alt+Shift+T";
    }
    return nullptr;
}
#else
constexpr char storageKey[] = "keymap";
#endif
constexpr char wifiPassword[] = "castsetup";
constexpr char tokenHeader[] = "X-Setup-Token";

bool validEventName(const char* name, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        const char c = name[i];
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!letter && !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.' && c != '+') {
            return false;
        }
    }
    return true;
}

}  // namespace

void appendJsonString(String& output, const char* value) {
    output += '"';
    for (; *value; ++value) {
        if (*value == '"' || *value == '\\') output += '\\';
        output += static_cast<uint8_t>(*value) < 0x20 ? '?' : *value;
    }
    output += '"';
}

void SetupPortal::begin(Preferences* storage, String (*remoteStatus)(),
                        const char* (*remoteAction)(const String&, int), void (*beforeChange)()) {
    storage_ = storage;
    remoteStatus_ = remoteStatus;
    remoteAction_ = remoteAction;
    beforeChange_ = beforeChange;
    for (size_t i = 0; i < chromecast::keyCount; ++i) {
        strlcpy(mapping_.tty.names[i], chromecast::keys[i].name, nameSize);
#if defined(CHROMECAST_USB_MIC)
        strlcpy(mapping_.shortcuts[i], chromecast::keys[i].shortcut, keyboardShortcut::textSize);
#endif
    }
    Mapping saved;
    if (storage_) {
        if (storage_->getBytesLength(storageKey) == sizeof(saved) &&
            storage_->getBytes(storageKey, &saved, sizeof(saved)) == sizeof(saved)) {
            configured_ = validMapping(saved);
        }
#if defined(CHROMECAST_USB_MIC)
        if (!configured_ && storage_->getBytesLength(previousStorageKey) == keyMappingBytes) {
            saved = mapping_;
            configured_ = storage_->getBytes(previousStorageKey, &saved, keyMappingBytes) == keyMappingBytes &&
                          validMapping(saved);
            if (configured_) {
                for (size_t i = 0; i < chromecast::keyCount; ++i) {
                    if (const char* replacement = updatedShortcut(saved.shortcuts[i], chromecast::keys[i])) {
                        strlcpy(saved.shortcuts[i], replacement, keyboardShortcut::textSize);
                    }
                }
                auto& voiceShortcut = saved.shortcuts[chromecast::assistantIndex];
                if (!*voiceShortcut) {
                    strlcpy(voiceShortcut, chromecast::keys[chromecast::assistantIndex].shortcut,
                            keyboardShortcut::textSize);
                }
            }
        }
        if (!configured_ && storage_->getBytesLength("keymap") == sizeof(saved.tty)) {
            saved = mapping_;
            configured_ = storage_->getBytes("keymap", &saved.tty, sizeof(saved.tty)) == sizeof(saved.tty) &&
                          validMapping(saved);
        }
#endif
        if (configured_) mapping_ = saved;
    }
#if defined(CHROMECAST_USB_MIC)
    usbMic::setGainSteps(mapping_.micGainSteps);
#endif
    routes();
}

bool SetupPortal::validMapping(const Mapping& mapping) const {
    if (mapping.tty.version != mapping_.tty.version) return false;
    for (const auto& name : mapping.tty.names) {
        const size_t size = strnlen(name, nameSize);
        if (size == nameSize || !validEventName(name, size)) return false;
    }
#if defined(CHROMECAST_USB_MIC)
    if (!usbMic::validGainSteps(mapping.micGainSteps)) return false;
    for (const auto& text : mapping.shortcuts) {
        keyboardShortcut::Chord chord;
        if (strnlen(text, sizeof(text)) == sizeof(text) || !keyboardShortcut::parse(text, chord)) return false;
    }
#endif
    return true;
}

bool SetupPortal::open() {
    if (active_) return true;
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "Chromecast-Setup-%04X", unsigned(ESP.getEfuseMac() >> 32) & 0xffff);
    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP) || !WiFi.softAP(ssid, wifiPassword, 1, false, 2)) {
        WiFi.mode(WIFI_OFF);
        tty().println("ERR setup WiFi could not start");
        return false;
    }
    address_ = WiFi.softAPIP().toString();
    char token[17];
    snprintf(token, sizeof(token), "%08lX%08lX", static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    token_ = token;
    lastKey_ = lastAction_ = "";
    closing_ = false;
    active_ = true;
    server_.begin();
    tty().printf("SETUP WiFi=%s password=%s url=http://%s\n", ssid, wifiPassword, address_.c_str());
    return true;
}

void SetupPortal::poll() {
    if (!active_) return;
    if (!closing_) {
        server_.handleClient();
        return;
    }
    if (static_cast<int32_t>(millis() - closeAt_) < 0) return;
    server_.stop();
    WiFi.softAPdisconnect(true);
    if (WiFi.getMode() != WIFI_OFF && !WiFi.mode(WIFI_OFF)) {
        tty().println("WARN WiFi shutdown will be retried");
        closeAt_ = millis() + 1000;
        return;
    }
    active_ = false;
    closing_ = false;
    tty().println("SETUP complete wifi=off");
}

void SetupPortal::recordKey(const char* key, const char* action) {
    if (!active_ || closing_) return;
    lastKey_ = key;
    lastAction_ = action;
}

void SetupPortal::routes() {
    const char* headers[] = {tokenHeader};
    server_.collectHeaders(headers, 1);
    server_.on("/", HTTP_GET, [this]() {
        if (!requestAllowed(false)) return;
        server_.sendHeader("Cache-Control", "no-store");
        server_.send_P(200, "text/html; charset=utf-8", setupPage);
    });
    server_.on("/api/config", HTTP_GET, [this]() {
        if (requestAllowed(false)) sendConfig();
    });
    server_.on("/api/status", HTTP_GET, [this]() {
        if (requestAllowed(false)) sendStatus();
    });
    server_.on("/api/remote", HTTP_POST, [this]() {
        if (!requestAllowed(true)) return;
        const String action = server_.arg("action");
        if (action != "scan" && action != "connect" && action != "disconnect") {
            sendJson(400, "{\"error\":\"invalid_action\"}");
            return;
        }
        const String value = server_.arg("id");
        char* end;
        const long id = strtol(value.c_str(), &end, 10);
        if (action == "connect" && (*end || id < 1 || id > 48)) {
            sendJson(400, "{\"error\":\"invalid_device\"}");
            return;
        }
        const char* error = remoteAction_(action, static_cast<int>(id));
        if (error) {
            String response = "{\"error\":";
            appendJsonString(response, error);
            response += '}';
            sendJson(409, response);
            return;
        }
        sendStatus();
    });
    server_.on("/api/save", HTTP_POST, [this]() {
        if (requestAllowed(true)) save();
    });
    server_.onNotFound([this]() { sendJson(404, "{\"error\":\"not_found\"}"); });
}

bool SetupPortal::requestAllowed(bool write) {
    const String host = server_.hostHeader();
    if ((host != address_ && host != address_ + ":80") || (write && server_.header(tokenHeader) != token_)) {
        sendJson(403, "{\"error\":\"forbidden\"}");
        return false;
    }
    return true;
}

void SetupPortal::sendJson(int status, const String& payload) {
    server_.sendHeader("Cache-Control", "no-store");
    server_.send(status, "application/json; charset=utf-8", payload);
}

void SetupPortal::sendConfig() {
    String payload = "{\"token\":";
    payload.reserve(sizeof(Mapping) + 1024);
    appendJsonString(payload, token_.c_str());
#if defined(CHROMECAST_USB_MIC)
    payload += ",\"shortcutLimit\":" + String(keyboardShortcut::textSize - 1);
    payload += ",\"micGainSteps\":" + String(unsigned(mapping_.micGainSteps));
    payload += ",\"micGainOptions\":[";
    for (size_t i = 0; i < sizeof(usbMic::gainOptions); ++i) {
        if (i) payload += ',';
        payload += String(unsigned(usbMic::gainOptions[i]));
    }
    payload += ']';
#endif
    payload += ",\"keys\":[";
    for (size_t i = 0; i < chromecast::keyCount; ++i) {
        if (i) payload += ',';
        payload += "{\"key\":";
        appendJsonString(payload, chromecast::keys[i].name);
        payload += ",\"name\":";
        appendJsonString(payload, mapping_.tty.names[i]);
#if defined(CHROMECAST_USB_MIC)
        payload += ",\"shortcut\":";
        appendJsonString(payload, mapping_.shortcuts[i]);
        payload += ",\"defaultShortcut\":";
        appendJsonString(payload, chromecast::keys[i].shortcut);
#endif
        payload += '}';
    }
    payload += "]}";
    sendJson(200, payload);
}

void SetupPortal::sendStatus() {
    String payload = "{\"remote\":";
    payload += remoteStatus_();
    payload += ",\"lastKey\":";
    appendJsonString(payload, lastKey_);
    payload += ",\"lastAction\":";
    appendJsonString(payload, lastAction_);
    payload += '}';
    sendJson(200, payload);
}

void SetupPortal::save() {
    Mapping updated;
#if defined(CHROMECAST_USB_MIC)
    updated.micGainSteps = mapping_.micGainSteps;
    if (server_.hasArg("micGainSteps")) {
        const String value = server_.arg("micGainSteps");
        char* end;
        const unsigned long steps = strtoul(value.c_str(), &end, 10);
        if (end != value.c_str() + value.length() || !usbMic::validGainSteps(steps)) {
            sendJson(400, "{\"error\":\"invalid_mic_gain\"}");
            return;
        }
        updated.micGainSteps = static_cast<uint8_t>(steps);
    }
#endif
    for (size_t i = 0; i < chromecast::keyCount; ++i) {
        const String field = "k" + String(i);
        const String name = server_.arg(field);
        if (!server_.hasArg(field) || name.length() >= nameSize || !validEventName(name.c_str(), name.length())) {
            sendJson(400, "{\"error\":\"invalid_mapping\",\"index\":" + String(i) + "}");
            return;
        }
        strlcpy(updated.tty.names[i], name.c_str(), nameSize);
#if defined(CHROMECAST_USB_MIC)
        const String shortcutField = "h" + String(i);
        const String text = server_.arg(shortcutField);
        keyboardShortcut::Chord chord;
        if (!server_.hasArg(shortcutField) || text.length() >= keyboardShortcut::textSize ||
            strlen(text.c_str()) != text.length() || !keyboardShortcut::parse(text.c_str(), chord)) {
            sendJson(400, "{\"error\":\"invalid_shortcut\",\"index\":" + String(i) + "}");
            return;
        }
        strlcpy(updated.shortcuts[i], text.c_str(), keyboardShortcut::textSize);
#endif
    }
    if (!storage_ || storage_->putBytes(storageKey, &updated, sizeof(updated)) != sizeof(updated)) {
        sendJson(500, "{\"error\":\"flash_write\"}");
        return;
    }
    if (memcmp(&mapping_, &updated, keyMappingBytes) != 0) beforeChange_();
    mapping_ = updated;
#if defined(CHROMECAST_USB_MIC)
    usbMic::setGainSteps(mapping_.micGainSteps);
#endif
    configured_ = true;
    sendJson(200, "{\"saved\":true}");
    closing_ = true;
    closeAt_ = millis() + 1200;
}
