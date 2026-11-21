#include "usb_microphone.h"

#if defined(CHROMECAST_USB_MIC)

#include <USB.h>
#include <esp32-hal-tinyusb.h>
#include <tusb.h>
#include <device/usbd_pvt.h>

#include <algorithm>
#include <cmath>

#include "usb_audio_descriptor.h"
#include "usb_audio_gain.h"
#include "usb_keyboard.h"

namespace {

class AudioConsole final : public USBCDC {
public:
    size_t write(uint8_t byte) override { return write(&byte, 1); }

    size_t write(const uint8_t* bytes, size_t size) override {
        // A stalled serial reader must not hold up BLE audio handling.
        const uint32_t start = millis();
        size_t sent = 0;
        while (sent < size && tud_cdc_n_connected(0)) {
            const size_t room = tud_cdc_n_write_available(0);
            const size_t written = room ? tud_cdc_n_write(0, bytes + sent, std::min(room, size - sent)) : 0;
            sent += written;
            tud_cdc_n_write_flush(0);
            if (millis() - start >= 8) break;
            if (!written) delay(1);
        }
        return sent;
    }
};

}  // namespace

USBCDC& tty() {
    static AudioConsole port;
    return port;
}

namespace usbMic {
namespace {

constexpr uint16_t fifoSize = 2048;   // 128 ms at 16 kHz
constexpr uint16_t prefill = 480;     // 30 ms of BLE jitter before the first USB packet
constexpr int16_t minVolume = -60 * 256;
constexpr int16_t maxVolume = 0;
constexpr int16_t volumeStep = 256;

portMUX_TYPE audioLock = portMUX_INITIALIZER_UNLOCKED;
int16_t fifo[fifoSize];
uint16_t readIndex = 0;
uint16_t count = 0;
bool receiving = false;
bool primed = false;
bool usbStreaming = false;  // Protected by audioLock for the BLE/application task.
uint8_t gainSteps = unityGainSteps;

uint8_t controlInterface = 0xff;
uint8_t streamingInterface = 0xff;
uint8_t endpointAddress = 0;
uint8_t alternate = 0;
bool endpointOpen = false;
const tusb_desc_endpoint_t* endpoint = nullptr;
alignas(4) int16_t packet[samplesPerPacket];
uint8_t mute = 0;
int16_t volume = 0;
int32_t gain = 32768;
uint8_t controlData[2];

void updateStreaming(bool enabled) {
    portENTER_CRITICAL(&audioLock);
    usbStreaming = enabled;
    readIndex = count = 0;
    primed = false;
    portEXIT_CRITICAL(&audioLock);
}

void fillPacket() {
    portENTER_CRITICAL(&audioLock);
    const uint8_t boost = gainSteps;
    primed = primed || count >= prefill || !receiving;
    for (auto& sample : packet) {
        sample = 0;
        if (primed && count) {
            sample = fifo[readIndex];
            readIndex = (readIndex + 1) % fifoSize;
            --count;
        }
    }
    if (!count) primed = false;
    portEXIT_CRITICAL(&audioLock);

    for (auto& sample : packet) {
        sample = mute ? 0 : applyGain(sample, gain, boost);
    }
}

bool sendPacket(uint8_t port) {
    if (!usbd_edpt_claim(port, endpointAddress)) return false;
    fillPacket();
    return usbd_edpt_xfer(port, endpointAddress, reinterpret_cast<uint8_t*>(packet), sizeof(packet));
}

void reset(uint8_t) {
    alternate = 0;
    endpointOpen = false;
    endpoint = nullptr;
    updateStreaming(false);
}

void init() { reset(0); }

uint16_t open(uint8_t, const tusb_desc_interface_t* interface, uint16_t maxLength) {
    constexpr uint16_t length = sizeof(AudioDescriptor) - associationBytes;
    if (interface->bInterfaceClass != TUSB_CLASS_AUDIO || interface->bInterfaceSubClass != 1 ||
        interface->bInterfaceProtocol != 0 || interface->bInterfaceNumber != controlInterface ||
        maxLength < length) return 0;
    const auto* descriptor = reinterpret_cast<const uint8_t*>(interface) + endpointOffset - associationBytes;
    endpoint = reinterpret_cast<const tusb_desc_endpoint_t*>(descriptor);
    return length;
}

bool interfaceRequest(uint8_t port, uint8_t stage, const tusb_control_request_t* request) {
    const uint8_t number = uint8_t(request->wIndex);
    if (number != controlInterface && number != streamingInterface) return false;
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bRequest == TUSB_REQ_GET_INTERFACE) {
        controlData[0] = number == streamingInterface ? alternate : 0;
        return tud_control_xfer(port, request, controlData, 1);
    }
    if (request->bRequest != TUSB_REQ_SET_INTERFACE || request->wValue > 1 ||
        (number == controlInterface && request->wValue != 0)) return false;
    if (number == streamingInterface && alternate != request->wValue) {
        if (request->wValue == 1) {
            if (!endpointOpen) {
                if (!endpoint || !usbd_edpt_open(port, endpoint)) return false;
                endpointOpen = true;
            }
            alternate = 1;
            updateStreaming(true);
            // A transfer already queued before alt 0 completes when the host resumes polling.
            if (!usbd_edpt_busy(port, endpointAddress) && !sendPacket(port)) {
                alternate = 0;
                updateStreaming(false);
                return false;
            }
        } else {
            alternate = 0;
            updateStreaming(false);
            // The pinned ESP32-S3 DCD closes endpoints on bus reset, not individually.
            // Leave its FIFO allocated and stop scheduling until alt 1 is selected again.
        }
    }
    return tud_control_status(port, request);
}

bool featureRequest(uint8_t port, uint8_t stage, const tusb_control_request_t* request) {
    if (request->wIndex != ((featureUnit << 8) | controlInterface) || (request->wValue & 0xff) != 0) return false;
    const uint8_t selector = request->wValue >> 8;
    if (selector != 1 && selector != 2) return false;
    const bool isMute = selector == 1;
    const uint8_t size = isMute ? 1 : 2;
    if (request->bRequest == 0x01) {  // SET_CUR
        if (request->wLength != size) return false;
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(port, request, controlData, size);
        if (stage == CONTROL_STAGE_DATA) {
            if (isMute) mute = controlData[0] != 0;
            else {
                const int16_t requested = controlData[0] | (uint16_t(controlData[1]) << 8);
                volume = std::max(minVolume, std::min(maxVolume, requested));
                gain = int32_t(std::pow(10.0f, float(volume) / (20 * 256)) * 32768.0f);
            }
        }
        return true;
    }
    if (stage != CONTROL_STAGE_SETUP) return true;
    int16_t value;
    switch (request->bRequest) {
        case 0x81: value = isMute ? mute : volume; break;  // GET_CUR
        case 0x82: if (isMute) return false; value = minVolume; break;
        case 0x83: if (isMute) return false; value = maxVolume; break;
        case 0x84: if (isMute) return false; value = volumeStep; break;
        default: return false;
    }
    controlData[0] = uint8_t(value);
    controlData[1] = uint8_t(uint16_t(value) >> 8);
    return tud_control_xfer(port, request, controlData, size);
}

bool control(uint8_t port, uint8_t stage, const tusb_control_request_t* request) {
    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) return interfaceRequest(port, stage, request);
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS) return featureRequest(port, stage, request);
    return false;
}

bool transferred(uint8_t port, uint8_t address, xfer_result_t, uint32_t) {
    if (address != endpointAddress) return false;
    if (alternate == 1) sendPacket(port);
    return true;
}

uint16_t loadDescriptor(uint8_t* destination, uint8_t* nextInterface) {
    const uint8_t ep = tinyusb_get_free_in_endpoint();
    if (!ep) return 0;
    endpointAddress = 0x80 | ep;
    controlInterface = (*nextInterface)++;
    streamingInterface = (*nextInterface)++;
    const auto descriptor = audioDescriptor(controlInterface, streamingInterface, endpointAddress,
                                            tinyusb_add_string_descriptor("Chromecast Remote Microphone"));
    memcpy(destination, descriptor.data, sizeof(descriptor));
    return sizeof(descriptor);
}

const usbd_class_driver_t driver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    "UAC1 microphone",
#endif
    init, reset, open, control, transferred, nullptr
};

}  // namespace

bool begin() {
    // Register CDC, HID and UAC before starting the same USB OTG device.
    tty().begin(115200);
    tty().enableReboot(false);
    usbKeyboard::begin();
    USB.productName("Chromecast Remote Microphone");
    USB.firmwareVersion(0x0500);
    USB.usbClass(TUSB_CLASS_MISC);
    USB.usbSubClass(MISC_SUBCLASS_COMMON);
    USB.usbProtocol(MISC_PROTOCOL_IAD);
    const bool audio = tinyusb_enable_interface(USB_INTERFACE_CUSTOM, sizeof(AudioDescriptor), loadDescriptor) == ESP_OK;
    return USB.begin() && audio;
}

bool listening() {
    portENTER_CRITICAL(&audioLock);
    const bool active = usbStreaming;
    portEXIT_CRITICAL(&audioLock);
    return active;
}

void setGainSteps(uint8_t steps) {
    portENTER_CRITICAL(&audioLock);
    gainSteps = steps;
    portEXIT_CRITICAL(&audioLock);
}

void startAudio() {
    portENTER_CRITICAL(&audioLock);
    readIndex = count = 0;
    primed = false;
    receiving = true;
    portEXIT_CRITICAL(&audioLock);
}

void pushAudio(const int16_t* samples, size_t size) {
    if (size > fifoSize) {
        samples += size - fifoSize;
        size = fifoSize;
    }
    portENTER_CRITICAL(&audioLock);
    if (usbStreaming && receiving) {
        const int excess = int(count) + int(size) - fifoSize;
        if (excess > 0) {
            readIndex = (readIndex + excess) % fifoSize;
            count -= excess;
        }
        for (size_t i = 0; i < size; ++i) fifo[(readIndex + count + i) % fifoSize] = samples[i];
        count += size;
    }
    portEXIT_CRITICAL(&audioLock);
}

void stopAudio(bool discard) {
    portENTER_CRITICAL(&audioLock);
    receiving = false;
    if (discard) {
        readIndex = count = 0;
        primed = false;
    }
    portEXIT_CRITICAL(&audioLock);
}

}  // namespace usbMic

extern "C" const usbd_class_driver_t* usbd_app_driver_get_cb(uint8_t* count) {
    *count = 1;
    return &usbMic::driver;
}

#endif
