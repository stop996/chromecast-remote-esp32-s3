#include <Arduino.h>
#include <Preferences.h>
#include <esp32-hal-bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gatt_common_api.h>
#include <esp_gattc_api.h>

#include <algorithm>
#include <string.h>
#include <vector>

#include "chromecast_keys.h"
#include "atvv_voice.h"
#include "setup_portal.h"
#include "usb_keyboard.h"
#include "usb_microphone.h"

namespace {

constexpr uint16_t hidService = 0x1812;
constexpr uint16_t reportMap = 0x2a4b;
constexpr uint16_t hidReport = 0x2a4d;
constexpr uint16_t reportReference = 0x2908;
constexpr uint16_t cccd = 0x2902;
constexpr uint8_t atvvService[] = {0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                                   0x05, 0x4f, 0x21, 0x5a, 0x01, 0x00, 0x5e, 0xab};

bool isAtvvCharacteristic(const esp_bt_uuid_t& uuid, uint8_t number) {
    if (uuid.len != ESP_UUID_LEN_128) return false;
    const auto* bytes = uuid.uuid.uuid128;
    return bytes[12] == number && !memcmp(bytes, atvvService, 12) &&
           !memcmp(bytes + 13, atvvService + 13, 3);
}

struct Peer {
    esp_bd_addr_t address{};
    uint8_t type = BLE_ADDR_TYPE_PUBLIC;
};

bool sameAddress(const Peer& a, const Peer& b) {
    return memcmp(a.address, b.address, sizeof(a.address)) == 0;
}

String addressText(const Peer& peer) {
    char address[18];
    const auto* b = peer.address;
    snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    return address;
}

enum class EventType {
    Registered, ScanConfigured, ScanStarted, Advertisement, ScanDone, Open, Auth,
    Service, ServicesDone, Characteristic, Descriptor, RegisteredForNotify, Written,
    Report, Disconnected,
};

struct Event {
    explicit Event(EventType kind) : type(kind) {}
    EventType type;
    int status = 0;
    uint16_t conn = 0;
    uint16_t handle = 0;
    uint16_t end = 0;
    uint16_t size = 0;
    Peer peer;
    uint8_t bytes[ESP_GATT_MAX_ATTR_LEN]{};
};

QueueHandle_t events;

void post(const Event& event) {
    // Leave room for link and discovery events on a busy radio channel.
    if (event.type == EventType::Advertisement && uxQueueSpacesAvailable(events) < 8) return;
    xQueueSend(events, &event, pdMS_TO_TICKS(20));
}

void postValue(EventType type, uint16_t conn, uint16_t handle, int status,
               const uint8_t* bytes, uint16_t size) {
    Event event(type);
    event.conn = conn;
    event.handle = handle;
    event.status = status;
    if (bytes && size && size <= sizeof(event.bytes)) {
        event.size = size;
        memcpy(event.bytes, bytes, size);
    }
    post(event);
}

void gapCallback(esp_gap_ble_cb_event_t kind, esp_ble_gap_cb_param_t* p) {
    switch (kind) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
            Event event(EventType::ScanConfigured);
            event.status = p->scan_param_cmpl.status;
            post(event);
            break;
        }
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: {
            Event event(EventType::ScanStarted);
            event.status = p->scan_start_cmpl.status;
            post(event);
            break;
        }
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            const auto& result = p->scan_rst;
            if (result.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
                post(Event(EventType::ScanDone));
                break;
            }
            if (result.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT ||
                (result.ble_evt_type != ESP_BLE_EVT_CONN_ADV &&
                 result.ble_evt_type != ESP_BLE_EVT_CONN_DIR_ADV &&
                 result.ble_evt_type != ESP_BLE_EVT_SCAN_RSP)) break;
            Event event(EventType::Advertisement);
            memcpy(event.peer.address, result.bda, sizeof(event.peer.address));
            event.peer.type = result.ble_addr_type;
            event.status = result.rssi;
            event.handle = result.adv_data_len;
            event.size = std::min<size_t>(result.adv_data_len + result.scan_rsp_len,
                                          sizeof(result.ble_adv));
            memcpy(event.bytes, result.ble_adv, event.size);
            post(event);
            break;
        }
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
            break;
        case ESP_GAP_BLE_NC_REQ_EVT:
            esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr, false);
            break;
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            esp_ble_passkey_reply(p->ble_security.ble_req.bd_addr, false, 0);
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            const auto& auth = p->ble_security.auth_cmpl;
            Event event(EventType::Auth);
            memcpy(event.peer.address, auth.bd_addr, sizeof(event.peer.address));
            event.peer.type = auth.addr_type;
            event.status = auth.success ? 0 : auth.fail_reason;
            post(event);
            break;
        }
        default:
            break;
    }
}

void gattCallback(esp_gattc_cb_event_t kind, esp_gatt_if_t gattIf, esp_ble_gattc_cb_param_t* p) {
    switch (kind) {
        case ESP_GATTC_REG_EVT: {
            Event event(EventType::Registered);
            event.status = p->reg.status;
            event.handle = gattIf;
            post(event);
            break;
        }
        case ESP_GATTC_OPEN_EVT: {
            Event event(EventType::Open);
            event.status = p->open.status;
            event.conn = p->open.conn_id;
            memcpy(event.peer.address, p->open.remote_bda, sizeof(event.peer.address));
            post(event);
            break;
        }
        case ESP_GATTC_SEARCH_RES_EVT: {
            const auto& uuid = p->search_res.srvc_id.uuid;
            const bool hid = uuid.len == ESP_UUID_LEN_16 && uuid.uuid.uuid16 == hidService;
            const bool atvv = uuid.len == ESP_UUID_LEN_128 &&
                              memcmp(uuid.uuid.uuid128, atvvService, sizeof(atvvService)) == 0;
            if (!hid && !atvv) break;
            Event event(EventType::Service);
            event.status = hid ? hidService : 0;
            event.conn = p->search_res.conn_id;
            event.handle = p->search_res.start_handle;
            event.end = p->search_res.end_handle;
            post(event);
            break;
        }
        case ESP_GATTC_SEARCH_CMPL_EVT:
            postValue(EventType::ServicesDone, p->search_cmpl.conn_id, 0,
                      p->search_cmpl.status, nullptr, 0);
            break;
        case ESP_GATTC_READ_CHAR_EVT:
        case ESP_GATTC_READ_DESCR_EVT:
            postValue(kind == ESP_GATTC_READ_CHAR_EVT ? EventType::Characteristic : EventType::Descriptor,
                      p->read.conn_id, p->read.handle, p->read.status, p->read.value, p->read.value_len);
            break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            postValue(EventType::RegisteredForNotify, 0, p->reg_for_notify.handle,
                      p->reg_for_notify.status, nullptr, 0);
            break;
        case ESP_GATTC_WRITE_DESCR_EVT:
            postValue(EventType::Written, p->write.conn_id, p->write.handle, p->write.status, nullptr, 0);
            break;
        case ESP_GATTC_NOTIFY_EVT:
            postValue(EventType::Report, p->notify.conn_id, p->notify.handle, 0,
                      p->notify.value, p->notify.value_len);
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            postValue(EventType::Disconnected, p->disconnect.conn_id, 0, p->disconnect.reason, nullptr, 0);
            break;
        default:
            break;
    }
}

struct Device {
    Peer peer;
    String name;
    int rssi = 0;
    bool hid = false;
};

struct Service {
    uint16_t start = 0;
    uint16_t end = 0;
};

enum class InputKind { Hid, AtvvControl, AtvvAudio };

const char* inputName(InputKind kind) {
    if (kind == InputKind::AtvvControl) return "ATVV_CTL";
    if (kind == InputKind::AtvvAudio) return "ATVV_AUDIO";
    return "2A4D";
}

struct Input {
    uint16_t handle = 0;
    uint16_t reference = 0;
    uint16_t subscription = 0;
    int id = -1;
    uint8_t cccValue = 1;
    InputKind kind = InputKind::Hid;
    bool subscribed = false;
};

enum class State { Starting, Idle, Scanning, Connecting, Discovering, Subscribing, Ready, Closing };
enum class ScanMode { List, Pair, Reconnect };

State state = State::Starting;
ScanMode scanMode = ScanMode::Pair;
uint32_t deadline = 0;
uint32_t nextScan = 0;
bool autoReconnect = true;
bool forgetOnClose = false;
bool storageReady = false;
bool hasSaved = false;
bool hasAttempt = false;
bool hasLink = false;
uint8_t hidSelectorCount = 0;
bool namedChromecast = false;
bool paddedHidReports = false;
Preferences storage;
SetupPortal portal;
Peer savedPeer;
Peer attemptedPeer;
Peer linkPeer;
esp_gatt_if_t client = ESP_GATT_IF_NONE;
uint16_t connection = 0;
Service hid;
Service atvv;
uint16_t mapHandle = 0;
std::vector<Device> devices;
std::vector<Input> inputs;
size_t inputIndex = 0;
uint32_t pressed = 0;

const char* keyDecoder() {
    if (hidSelectorCount) return "chromecast-9450";
    return paddedHidReports ? "chromecast-padded" : "raw";
}

#if defined(CHROMECAST_USB_MIC)
AtvvVoice voice;
bool voiceKeyDown = false;
bool voiceKeyUsesHid = false;
uint16_t voiceCommand = 0;
esp_gatt_write_type_t voiceWriteType = ESP_GATT_WRITE_TYPE_NO_RSP;
constexpr uint8_t setupButton = 0;  // Waveshare Tiny BOOT button; sampled only after ROM boot.
uint32_t setupWindowUntil = 0;
uint32_t setupHoldAt = 0;

bool writeVoiceCommand(const uint8_t* bytes, size_t size) {
    uint8_t command[8];
    if (!hasLink || !voiceCommand || size > sizeof(command)) return false;
    memcpy(command, bytes, size);
    return esp_ble_gattc_write_char(client, connection, voiceCommand, size, command,
                                   voiceWriteType, ESP_GATT_AUTH_REQ_NO_MITM) == ESP_OK;
}
#endif

bool due(uint32_t time) {
    return static_cast<int32_t>(millis() - time) >= 0;
}

#if defined(CHROMECAST_USB_MIC)
void pollSetupButton() {
    if (!setupWindowUntil) return;
    if (due(setupWindowUntil)) {
        setupWindowUntil = 0;
        return;
    }
    if (digitalRead(setupButton) != LOW) {
        setupHoldAt = 0;
        return;
    }
    if (!setupHoldAt) setupHoldAt = millis();
    if (due(setupHoldAt + 650)) {
        setupWindowUntil = 0;
        portal.open();
    }
}
#endif

void enter(State next, uint32_t timeoutMs = 0) {
    state = next;
    deadline = timeoutMs ? millis() + timeoutMs : 0;
}

void showBytes(const uint8_t* data, uint16_t size) {
    static constexpr char digits[] = "0123456789ABCDEF";
    char hex[3 * ESP_GATT_MAX_ATTR_LEN + 1];
    size_t end = 0;
    for (uint16_t i = 0; i < size; ++i) {
        if (i) hex[end++] = ' ';
        hex[end++] = digits[data[i] >> 4];
        hex[end++] = digits[data[i] & 0xf];
    }
    hex[end] = '\0';
    tty().println(hex);
}

void reportKey(const chromecast::Key& key, bool down) {
    if (down) portal.recordKey(key.name, "down");
    const size_t index = &key - chromecast::keys;
    const char* name = portal.eventName(index);
    if (*name) {
        tty().printf("KEY %lu %s %s usage=0C:%04X\n", millis(), down ? "down" : "up", name, key.usage);
    }
}

#if defined(CHROMECAST_USB_MIC)
void voiceKeyEvent(bool down) {
    if (voiceKeyDown == down) return;
    voiceKeyDown = down;
    const auto& key = chromecast::keys[chromecast::assistantIndex];
    if (!down) portal.recordKey(key.name, "up");
    reportKey(key, down);
    usbKeyboard::holdVoice(portal.shortcut(chromecast::assistantIndex), down);
}
#endif

void keyEvent(const chromecast::Key& key, bool down) {
#if defined(CHROMECAST_USB_MIC)
    const size_t index = &key - chromecast::keys;
    if (index == chromecast::assistantIndex) {
        voiceKeyUsesHid = true;
        voiceKeyEvent(down);
        return;
    }
    if (down) usbKeyboard::trigger(portal.shortcut(index));
#endif
    reportKey(key, down);
}

void releasePressed() {
    chromecast::changes(pressed, 0, keyEvent);
    pressed = 0;
#if defined(CHROMECAST_USB_MIC)
    voiceKeyEvent(false);
    usbKeyboard::cancel();
#endif
}

void forgetPeer() {
    forgetOnClose = false;
    if (storageReady && storage.isKey("peer") && !storage.remove("peer")) {
        tty().println("ERR could not remove saved peer from flash");
        return;
    }
    if (hasSaved || hasAttempt) {
        Peer target = hasSaved ? savedPeer : attemptedPeer;
        if (esp_ble_remove_bond_device(target.address) != ESP_OK) {
            tty().println("WARN could not request removal of the Bluetooth bond");
        }
    }
    hasSaved = false;
    hasAttempt = false;
    tty().println("BLE forgotten. Put the remote into pairing mode and send pair.");
}

void returnToIdle() {
    releasePressed();
#if defined(CHROMECAST_USB_MIC)
    if (hasLink) voice.disconnect();
#endif
    hasLink = false;
    enter(State::Idle);
    nextScan = millis() + 2000;
    if (forgetOnClose) forgetPeer();
}

void closeLink() {
    if (!hasLink) {
        returnToIdle();
        return;
    }
    enter(State::Closing, 4000);
    if (esp_ble_gattc_close(client, connection) != ESP_OK) esp_ble_gap_disconnect(linkPeer.address);
}

void failLink(const char* operation, int error) {
    tty().printf("ERR %s code=0x%X\n", operation, error);
    closeLink();
}

bool isChromecast(const Device& device) {
    return strcasestr(device.name.c_str(), "chromecast") != nullptr;
}

void connectTo(const Device& device) {
    attemptedPeer = device.peer;
    hasAttempt = true;
    hid = {};
    atvv = {};
    mapHandle = 0;
    hidSelectorCount = 0;
    namedChromecast = isChromecast(device) || strcasestr(device.name.c_str(), "google tv") != nullptr;
    paddedHidReports = false;
    inputs.clear();
#if defined(CHROMECAST_USB_MIC)
    voiceCommand = 0;
    voiceKeyUsesHid = false;
#endif
    tty().printf("BLE connecting addr=%s name=%s\n", addressText(attemptedPeer).c_str(), device.name.c_str());
    enter(State::Connecting, 35000);
    const auto error = esp_ble_gattc_open(client, attemptedPeer.address,
                                        static_cast<esp_ble_addr_type_t>(attemptedPeer.type), true);
    if (error != ESP_OK) failLink("open", error);
}

void startScan(ScanMode mode) {
    devices.clear();
    scanMode = mode;
    enter(State::Scanning, 7500);
    if (mode != ScanMode::Reconnect) tty().println("BLE scanning for 5 seconds");
    const auto error = esp_ble_gap_start_scanning(5);
    if (error != ESP_OK) failLink("scan", error);
}

void parseAdvertisingData(Device& device, const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size;) {
        const size_t length = bytes[i++];
        if (!length || length > size - i) break;
        const uint8_t type = bytes[i];
        if (length > 1 && (type == 0x09 || (type == 0x08 && device.name.isEmpty()))) {
            device.name = "";
            for (size_t j = 1; j < length; ++j) {
                const uint8_t c = bytes[i + j];
                device.name += c < 0x20 || c == 0x7f ? '?' : static_cast<char>(c);
            }
        }
        if (type == 0x02 || type == 0x03) {
            for (size_t j = 1; j + 1 < length; j += 2) {
                if ((bytes[i + j] | (bytes[i + j + 1] << 8)) == hidService) device.hid = true;
            }
        }
        i += length;
    }
}

void collectDevice(const Event& event) {
    if (scanMode == ScanMode::Reconnect && !sameAddress(savedPeer, event.peer)) return;
    auto it = std::find_if(devices.begin(), devices.end(), [&](const Device& device) {
        return sameAddress(device.peer, event.peer);
    });
    if (it == devices.end()) {
        if (devices.size() >= 48) {
            Device candidate;
            parseAdvertisingData(candidate, event.bytes, std::min(event.handle, event.size));
            if (event.handle < event.size) {
                parseAdvertisingData(candidate, event.bytes + event.handle, event.size - event.handle);
            }
            if (!isChromecast(candidate) && !candidate.hid) return;
            it = std::find_if(devices.begin(), devices.end(), [](const Device& device) {
                return !device.hid && !isChromecast(device);
            });
            if (it == devices.end()) return;
            *it = std::move(candidate);
        } else {
            devices.emplace_back();
            it = devices.end() - 1;
        }
    }
    it->peer = event.peer;
    it->rssi = event.status;
    const auto advSize = std::min(event.handle, event.size);
    parseAdvertisingData(*it, event.bytes, advSize);
    parseAdvertisingData(*it, event.bytes + advSize, event.size - advSize);
}

void finishScan() {
    returnToIdle();
    if (scanMode == ScanMode::Reconnect) {
        if (!devices.empty()) connectTo(devices.front());
        return;
    }
    size_t matches = 0;
    size_t match = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& device = devices[i];
        if (isChromecast(device)) {
            ++matches;
            match = i;
        }
        tty().printf("SCAN %u addr=%s rssi=%d hid=%d name=%s\n", unsigned(i + 1),
                      addressText(device.peer).c_str(), device.rssi, device.hid,
                      device.name.isEmpty() ? "(unnamed)" : device.name.c_str());
    }
    if (scanMode == ScanMode::Pair && matches == 1) {
        connectTo(devices[match]);
    } else {
        tty().println("BLE scan done. Select with connect N, or send pair to scan again.");
    }
}

uint16_t descriptor(uint16_t handle, uint16_t id) {
    esp_bt_uuid_t uuid{};
    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = id;
    esp_gattc_descr_elem_t result;
    uint16_t count = 1;
    if (esp_ble_gattc_get_descr_by_char_handle(client, connection, handle, uuid,
                                             &result, &count) == ESP_GATT_OK && count) {
        return result.handle;
    }
    return 0;
}

void findInputs(const Service& service, bool voiceService) {
    if (!service.start) return;
    uint16_t count = 0;
    if (esp_ble_gattc_get_attr_count(client, connection, ESP_GATT_DB_CHARACTERISTIC,
                                   service.start, service.end, 0, &count) != ESP_GATT_OK || !count) return;
    std::vector<esp_gattc_char_elem_t> chars(count);
    if (esp_ble_gattc_get_all_char(client, connection, service.start, service.end,
                                 chars.data(), &count, 0) != ESP_GATT_OK) return;
    Input controlInput;
    for (uint16_t i = 0; i < count; ++i) {
        const auto& ch = chars[i];
        const bool shortId = ch.uuid.len == ESP_UUID_LEN_16;
        Input input;
        if (voiceService) {
            if (isAtvvCharacteristic(ch.uuid, 0x04)) input.kind = InputKind::AtvvControl;
#if defined(CHROMECAST_USB_MIC)
            else if (isAtvvCharacteristic(ch.uuid, 0x03)) input.kind = InputKind::AtvvAudio;
            else if (isAtvvCharacteristic(ch.uuid, 0x02)) {
                if (ch.properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) {
                    voiceCommand = ch.char_handle;
                    voiceWriteType = ESP_GATT_WRITE_TYPE_NO_RSP;
                } else if (ch.properties & ESP_GATT_CHAR_PROP_BIT_WRITE) {
                    voiceCommand = ch.char_handle;
                    voiceWriteType = ESP_GATT_WRITE_TYPE_RSP;
                }
                continue;
            }
#endif
            else continue;
        } else {
            if (!shortId) continue;
            if (ch.uuid.uuid.uuid16 == reportMap) mapHandle = ch.char_handle;
            if (ch.uuid.uuid.uuid16 != hidReport) continue;
        }
        const bool notify = ch.properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        const bool indicate = ch.properties & ESP_GATT_CHAR_PROP_BIT_INDICATE;
        if (!notify && !indicate) continue;
        input.handle = ch.char_handle;
        input.reference = voiceService ? 0 : descriptor(ch.char_handle, reportReference);
        input.subscription = descriptor(ch.char_handle, cccd);
        input.cccValue = notify ? 1 : 2;
        if (!input.subscription) continue;
        if (input.kind == InputKind::AtvvControl) controlInput = input;
        else inputs.push_back(input);
    }
    // The remote may send the first audio frame as soon as control notifications are enabled.
    if (controlInput.subscription) inputs.push_back(controlInput);
}

void markReady() {
    size_t hidCount = 0;
    size_t voiceCount = 0;
    size_t audioCount = 0;
    for (const auto& input : inputs) {
        if (!input.subscribed) continue;
        if (input.kind == InputKind::Hid) ++hidCount;
        else if (input.kind == InputKind::AtvvControl) ++voiceCount;
        else ++audioCount;
    }
    if (!hidCount) {
        failLink("no HID input subscriptions", 0);
        return;
    }
    if (!hasSaved || !sameAddress(savedPeer, attemptedPeer) || savedPeer.type != attemptedPeer.type) {
        savedPeer = attemptedPeer;
        hasSaved = true;
        if (!storageReady || storage.putBytes("peer", &savedPeer, sizeof(savedPeer)) != sizeof(savedPeer)) {
            tty().println("WARN remote address is saved for this boot only");
        }
    }
    autoReconnect = true;
    enter(State::Ready);
    tty().printf("BLE READY hid=%u voice_control=%u decoder=%s\n", unsigned(hidCount),
                  unsigned(voiceCount), keyDecoder());
#if defined(CHROMECAST_USB_MIC)
    if (voiceCount && audioCount && voiceCommand) voice.connect(writeVoiceCommand);
    else tty().println("MIC unavailable: remote ATVV command, control or audio missing");
#endif
}

bool registerForInput(const Input& input) {
    const auto error = esp_ble_gattc_register_for_notify(client, linkPeer.address, input.handle);
    if (error != ESP_OK) tty().printf("WARN register handle=0x%04X code=0x%X\n", input.handle, error);
    return error == ESP_OK;
}

void subscribeNext() {
    enter(State::Subscribing, 15000);
    while (inputIndex < inputs.size()) {
        const auto& input = inputs[inputIndex];
        if (input.reference && esp_ble_gattc_read_char_descr(client, connection, input.reference,
                                                            ESP_GATT_AUTH_REQ_NO_MITM) == ESP_OK) return;
        if (registerForInput(input)) return;
        ++inputIndex;
    }
    markReady();
}

void discoverInputs() {
    findInputs(hid, false);
    findInputs(atvv, true);
    inputIndex = 0;
    if (mapHandle && esp_ble_gattc_read_char(client, connection, mapHandle,
                                            ESP_GATT_AUTH_REQ_NO_MITM) == ESP_OK) {
        enter(State::Discovering, 15000);
        return;
    }
    subscribeNext();
}

void handleSubscription(const Event& event) {
    if (state != State::Subscribing || inputIndex >= inputs.size()) return;
    auto& input = inputs[inputIndex];
    if (event.type == EventType::Descriptor && event.handle == input.reference) {
        if (event.status == ESP_GATT_OK && event.size == 2) {
            input.id = event.bytes[0];
            if (event.bytes[1] != 1) {
                ++inputIndex;
                subscribeNext();
                return;
            }
        } else {
            tty().printf("WARN report reference handle=0x%04X code=0x%X\n", event.handle, event.status);
        }
        if (registerForInput(input)) return;
    } else if (event.type == EventType::RegisteredForNotify && event.handle == input.handle) {
        int error = event.status;
        if (error == ESP_GATT_OK) {
            uint8_t value[] = {input.cccValue, 0};
            error = esp_ble_gattc_write_char_descr(client, connection, input.subscription,
                                                  sizeof(value), value, ESP_GATT_WRITE_TYPE_RSP,
                                                  ESP_GATT_AUTH_REQ_NO_MITM);
            if (error == ESP_OK) return;
        }
        tty().printf("WARN notify handle=0x%04X code=0x%X\n", input.handle, error);
    } else if (event.type == EventType::Written && event.handle == input.subscription) {
        input.subscribed = event.status == ESP_GATT_OK;
        tty().printf("BLE subscribe ch=%s handle=0x%04X report=%d status=0x%X\n",
                      inputName(input.kind), input.handle, input.id, event.status);
    } else {
        return;
    }
    ++inputIndex;
    subscribeNext();
}

void handleReport(const Event& event) {
    const auto it = std::find_if(inputs.begin(), inputs.end(), [&](const Input& input) {
        return event.handle == input.handle;
    });
    if (it == inputs.end()) return;
#if defined(CHROMECAST_USB_MIC)
    if (it->kind == InputKind::AtvvAudio) {
        voice.audio(event.bytes, event.size);
        return;
    }
#endif
    tty().printf("RAW %lu ch=%s handle=0x%04X report=%d len=%u data=", millis(),
                  inputName(it->kind), event.handle, it->id, event.size);
    showBytes(event.bytes, event.size);
    if (it->kind == InputKind::AtvvControl) {
        bool requested = event.size == 1 && event.bytes[0] == 0x08;
#if defined(CHROMECAST_USB_MIC)
        requested = voice.control(event.bytes, event.size) || requested;
        if (!voiceKeyUsesHid) {
            if (requested) voiceKeyEvent(true);
            if (event.size && event.bytes[0] == 0x00) voiceKeyEvent(false);
        }
#endif
        if (requested) {
#if !defined(CHROMECAST_USB_MIC)
            portal.recordKey(chromecast::keys[chromecast::assistantIndex].name, "request");
#endif
            const char* name = portal.eventName(chromecast::assistantIndex);
            if (*name) tty().printf("VOICE %lu request %s\n", millis(), name);
        }
        return;
    }
    uint32_t now;
    if (it->kind == InputKind::Hid &&
        chromecast::decodeReport(event.bytes, event.size, it->id, hidSelectorCount,
                                 namedChromecast && atvv.start, now)) {
        if (!hidSelectorCount && !paddedHidReports) {
            paddedHidReports = true;
            tty().printf("BLE decoder=%s report=%d len=%u\n", keyDecoder(), it->id, event.size);
        }
        chromecast::changes(pressed, now, keyEvent);
        pressed = now;
    }
}

void handleEvent(const Event& event) {
    switch (event.type) {
        case EventType::Registered:
            if (event.status) tty().printf("ERR GATT registration code=0x%X\n", event.status);
            else client = static_cast<esp_gatt_if_t>(event.handle);
            break;
        case EventType::ScanConfigured:
            if (event.status) tty().printf("ERR scan configuration code=0x%X\n", event.status);
            else enter(State::Idle);
            break;
        case EventType::ScanStarted:
            if (state == State::Scanning && event.status) failLink("scan start", event.status);
            break;
        case EventType::Advertisement:
            if (state == State::Scanning) collectDevice(event);
            break;
        case EventType::ScanDone:
            if (state == State::Scanning) finishScan();
            break;
        case EventType::Open: {
            if (state != State::Connecting) {
                if (event.status == ESP_GATT_OK) esp_ble_gattc_close(client, event.conn);
                break;
            }
            if (event.status != ESP_GATT_OK) {
                failLink("connection", event.status);
                break;
            }
            hasLink = true;
            connection = event.conn;
            linkPeer = event.peer;
            const auto security = esp_ble_set_encryption(linkPeer.address, ESP_BLE_SEC_ENCRYPT);
            if (security != ESP_OK) tty().printf("WARN encryption request code=0x%X\n", security);
            esp_ble_gattc_send_mtu_req(client, connection);
            enter(State::Discovering, 20000);
            const auto error = esp_ble_gattc_search_service(client, connection, nullptr);
            if (error != ESP_OK) failLink("service discovery", error);
            break;
        }
        case EventType::Auth:
            if (hasLink) tty().printf("BLE security addr=%s status=0x%X\n",
                                       addressText(event.peer).c_str(), event.status);
            break;
        case EventType::Service:
            if (state == State::Discovering && event.conn == connection) {
                auto& service = event.status == hidService ? hid : atvv;
                if (!service.start) {
                    service.start = event.handle;
                    service.end = event.end;
                }
            }
            break;
        case EventType::ServicesDone:
            if (state != State::Discovering || event.conn != connection) break;
            if (event.status != ESP_GATT_OK || !hid.start) failLink("HID service discovery", event.status);
            else discoverInputs();
            break;
        case EventType::Characteristic:
            if (state != State::Discovering || event.conn != connection || event.handle != mapHandle) break;
            hidSelectorCount = event.status == ESP_GATT_OK ? chromecast::selectorCount(event.bytes, event.size) : 0;
            tty().printf("HID_MAP status=0x%X len=%u data=", event.status, event.size);
            showBytes(event.bytes, event.size);
            subscribeNext();
            break;
        case EventType::Descriptor:
        case EventType::Written:
            if (event.conn != connection) break;
            handleSubscription(event);
            break;
        case EventType::RegisteredForNotify:
            handleSubscription(event);
            break;
        case EventType::Report:
            if ((state == State::Ready || state == State::Subscribing) && event.conn == connection) {
                handleReport(event);
            }
            break;
        case EventType::Disconnected:
            if (hasLink && event.conn == connection) {
                tty().printf("BLE disconnected reason=0x%X\n", event.status);
                returnToIdle();
            }
            break;
    }
}

void help() {
#if defined(CHROMECAST_USB_MIC)
    tty().println("Commands: pair | scan | connect N | status | disconnect | forget | help");
    tty().println("Setup: release RST, then hold BOOT during startup");
    tty().println("Microphone: mic status | mic start | mic stop");
    tty().println("USB keyboard: hid status");
#else
    tty().println("Commands: setup | pair | scan | connect N | status | disconnect | forget | help");
#endif
}

const char* stateName() {
    const char* names[] = {"starting", "idle", "scanning", "connecting", "discovering",
                           "subscribing", "ready", "closing"};
    return names[static_cast<size_t>(state)];
}

String remoteStatus() {
    String json = "{\"state\":\"";
    json.reserve(96 + devices.size() * 110);
    json += stateName();
    json += "\",\"decoder\":\"";
    if (state == State::Ready) json += keyDecoder();
    json += "\",\"devices\":[";
    if (state == State::Idle && scanMode != ScanMode::Reconnect) {
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto& device = devices[i];
            if (i) json += ',';
            json += "{\"id\":" + String(i + 1) + ",\"name\":";
            appendJsonString(json, device.name.c_str());
            json += ",\"address\":";
            appendJsonString(json, addressText(device.peer).c_str());
            json += device.hid ? ",\"hid\":true}" : ",\"hid\":false}";
        }
    }
    json += "]}";
    return json;
}

const char* remoteAction(const String& action, int number) {
    if (client == ESP_GATT_IF_NONE || (state != State::Idle && state != State::Ready)) {
        return "busy";
    }
    if (action == "disconnect" || action == "forget") {
        autoReconnect = false;
        forgetOnClose = action == "forget";
        closeLink();
    } else if (state == State::Ready) {
        return "connected";
    } else if (action == "pair" || action == "scan") {
        autoReconnect = false;
        startScan(action == "pair" ? ScanMode::Pair : ScanMode::List);
    } else if (action == "connect") {
        if (number < 1 || size_t(number) > devices.size()) return "invalid_device";
        autoReconnect = false;
        connectTo(devices[number - 1]);
    } else {
        return "invalid_action";
    }
    return nullptr;
}

void command(char* line) {
#if defined(CHROMECAST_USB_MIC)
    if (!strcmp(line, "hid status")) {
        const auto hidStatus = usbKeyboard::status();
        tty().printf("HID mounted=%d ready=%d queued=%u down=%lu up=%lu\n",
                     hidStatus.mounted, hidStatus.ready, unsigned(hidStatus.queued),
                     static_cast<unsigned long>(hidStatus.down), static_cast<unsigned long>(hidStatus.up));
        return;
    }
    if (!strcmp(line, "mic status")) { voice.status(); return; }
    if (!strcmp(line, "mic start")) {
        if (!voice.start()) tty().println("ERR MIC start unavailable");
        return;
    }
    if (!strcmp(line, "mic stop")) {
        if (!voice.stop()) tty().println("ERR MIC stop unavailable");
        return;
    }
#endif
    if (!strcmp(line, "help")) {
        help();
        return;
    }
    if (!strcmp(line, "setup")) {
        if (portal.active()) tty().println("SETUP already active");
#if defined(CHROMECAST_USB_MIC)
        else tty().println("SETUP release RST, then hold BOOT during startup");
#else
        else portal.open();
#endif
        return;
    }
    if (!strcmp(line, "status")) {
        tty().printf("STATUS state=%s saved=%s reconnect=%d setup=%s\n", stateName(),
                      hasSaved ? addressText(savedPeer).c_str() : "none", autoReconnect,
                      portal.active() ? "open" : "off");
        return;
    }
    const char* error;
    if (!strncmp(line, "connect ", 8)) {
        char* end;
        const long number = strtol(line + 8, &end, 10);
        error = (*end || number < 1 || number > 48) ? "invalid_device" : remoteAction("connect", number);
    } else {
        error = remoteAction(line, 0);
    }
    if (!error) return;
    if (!strcmp(error, "busy")) tty().println("BLE busy. Send status or retry when idle.");
    else if (!strcmp(error, "connected")) tty().println("BLE connected. Send disconnect to choose another remote.");
    else if (!strcmp(error, "invalid_device")) tty().println("ERR invalid scan number. Send scan first.");
    else help();
}

void readSerial() {
    static char line[48];
    static size_t length = 0;
    static bool overflow = false;
    while (tty().available()) {
        const int c = tty().read();
        if (c == '\r' || c == '\n') {
            if (overflow) tty().println("ERR command too long");
            else if (length) {
                line[length] = '\0';
                command(line);
            }
            overflow = false;
            length = 0;
        } else if ((c == 8 || c == 127) && length && !overflow) {
            --length;
        } else if (c >= 0x20 && c <= 0x7e && !overflow) {
            if (length < sizeof(line) - 1) line[length++] = static_cast<char>(c);
            else overflow = true;
        }
    }
}

bool initBle() {
    if (!btStart() || esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) return false;
    if (esp_ble_gap_register_callback(gapCallback) != ESP_OK ||
        esp_ble_gattc_register_callback(gattCallback) != ESP_OK) return false;
    esp_ble_gatt_set_local_mtu(185);
    uint8_t auth = ESP_LE_AUTH_REQ_SC_BOND;
    uint8_t capability = ESP_IO_CAP_NONE;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t size = 16;
    if (esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth)) != ESP_OK ||
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &capability, sizeof(capability)) != ESP_OK ||
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys)) != ESP_OK ||
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys)) != ESP_OK ||
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &size, sizeof(size)) != ESP_OK) return false;
    static esp_ble_scan_params_t scan{};
    scan.scan_type = BLE_SCAN_TYPE_ACTIVE;
    scan.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    scan.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    scan.scan_interval = 0x80;
    scan.scan_window = 0x50;
    scan.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
    return esp_ble_gattc_app_register(0) == ESP_OK && esp_ble_gap_set_scan_params(&scan) == ESP_OK;
}

}  // namespace

void setup() {
#if defined(CHROMECAST_USB_MIC)
    if (!usbMic::begin()) {
        Serial.begin(115200);
        Serial.println("ERR USB microphone initialization failed");
    }
#else
    tty().begin(115200);
#endif
    delay(500);
#if defined(CHROMECAST_USB_MIC)
    tty().println("\nChromecast Remote TTY, Microphone and Keyboard 0.5.0-gain1");
#else
    tty().println("\nChromecast Remote TTY 0.2.0");
#endif
    help();
    storageReady = storage.begin("cast-remote");
    if (storageReady && storage.getBytesLength("peer") == sizeof(savedPeer)) {
        hasSaved = storage.getBytes("peer", &savedPeer, sizeof(savedPeer)) == sizeof(savedPeer);
    }
    portal.begin(storageReady ? &storage : nullptr, remoteStatus, remoteAction, releasePressed);
    devices.reserve(48);
    events = xQueueCreate(32, sizeof(Event));
    if (!events || !initBle()) tty().println("ERR Bluetooth initialization failed");
    if (!portal.configured()) portal.open();
#if defined(CHROMECAST_USB_MIC)
    else {
        const auto reason = esp_reset_reason();
        if (reason == ESP_RST_POWERON || reason == ESP_RST_EXT) {
            pinMode(setupButton, INPUT_PULLUP);
            setupWindowUntil = millis() + 5000;
        }
    }
#endif
}

void loop() {
    Event event(EventType::Registered);
    if (events) {
        for (int i = 0; i < 32 && xQueueReceive(events, &event, 0) == pdTRUE; ++i) handleEvent(event);
    }
    readSerial();
    static bool firstScan = true;
    if (state == State::Idle && client != ESP_GATT_IF_NONE) {
        if (firstScan) {
            firstScan = false;
            if (autoReconnect && (hasSaved || !portal.active())) {
                startScan(hasSaved ? ScanMode::Reconnect : ScanMode::Pair);
            }
        } else if (hasSaved && autoReconnect && due(nextScan)) {
            startScan(ScanMode::Reconnect);
        }
    }
    if (deadline && due(deadline)) {
        if (state == State::Scanning) {
            esp_ble_gap_stop_scanning();
            failLink("scan timeout", 0);
        } else if (state == State::Closing || state == State::Connecting) {
            // Reset the stack if it never completed an open or close operation.
            tty().println("ERR BLE link timeout; restarting");
            delay(250);
            ESP.restart();
        } else {
            failLink("discovery/subscription timeout", 0);
        }
    }
#if defined(CHROMECAST_USB_MIC)
    voice.poll();
    if (!voiceKeyUsesHid && voiceKeyDown && !voice.active()) voiceKeyEvent(false);
    usbKeyboard::poll();
    pollSetupButton();
#endif
    portal.poll();
    delay(5);
}
