#include "atvv_voice.h"

#if defined(CHROMECAST_USB_MIC)

#include <algorithm>

#include "usb_microphone.h"

namespace {

constexpr uint8_t adpcm8k = 1;
constexpr uint8_t adpcm16k = 2;
constexpr size_t legacyHeader = 6;
constexpr uint8_t micOpen = 0x0c;
constexpr uint8_t micClose = 0x0d;
constexpr uint8_t micExtend = 0x0e;

uint16_t readBe16(const uint8_t* data) {
    return (uint16_t(data[0]) << 8) | data[1];
}

bool expired(uint32_t deadline) {
    return static_cast<int32_t>(millis() - deadline) >= 0;
}

}  // namespace

void AtvvVoice::connect(Write command) {
    disconnect();
    write_ = command;
    packets_ = samples_ = badFrames_ = 0;
    // The short capability command is the one used by the G9N9N capture in vibe-mote.
    const uint8_t chromecastCaps[] = {0x0a, 0x00, 0x04, 0x00, 0x01};
    if (write_(chromecastCaps, sizeof(chromecastCaps))) {
        tty().println("MIC ATVV subscribed; hold the remote voice key");
    } else {
        tty().println("WARN MIC capability command could not be sent");
    }
}

void AtvvVoice::disconnect() {
    finishCapture(true);
    write_ = nullptr;
    format_ = Format::Chromecast;
    preferredCodec_ = adpcm16k;
    legacySize_ = 0;
}

bool AtvvVoice::start() {
    if (!write_) return false;
    if (capture_ != Capture::Idle) return true;
    uint8_t command[] = {micOpen, 0, preferredCodec_};
    if (!write_(command, format_ == Format::Standard ? 2 : sizeof(command))) return false;
    searchPending_ = false;
    capture_ = Capture::Starting;
    lastAudio_ = millis();
    return true;
}

bool AtvvVoice::stop() {
    if (!write_) return false;
    const uint8_t command[] = {micClose, stream_};
    const bool sent = capture_ == Capture::Idle ||
                      write_(command, format_ == Format::Standard ? sizeof(command) : 1);
    finishCapture(false);
    return sent;
}

void AtvvVoice::capabilities(const uint8_t* data, size_t size) {
    if (size < 7 || capture_ != Capture::Idle) return;
    const uint16_t version = readBe16(data + 1);
    const auto frame = readBe16(data + 5);
    uint8_t codecs;
    if (version == 0x0100) {
        format_ = Format::Standard;
        codecs = data[3];
    } else if (version == 0x0004 && size >= 9 && data[3] == 0) {
        // The Chromecast variant uses raw 128-byte audio despite the short handshake.
        // Only select a legacy DVI layout when the response declares the larger framed size.
        if (frame >= 134 && frame <= sizeof(legacy_)) {
            format_ = Format::Legacy;
            legacySize_ = frame;
        }
        codecs = data[4];
    } else {
        return;
    }
    if (format_ != Format::Chromecast) {
        if (codecs & adpcm16k) preferredCodec_ = adpcm16k;
        else if (codecs & adpcm8k) preferredCodec_ = adpcm8k;
    }
    tty().printf("MIC caps version=%04X codecs=%02X frame=%u\n", version, codecs, frame);
}

void AtvvVoice::beginCapture(uint8_t codec, uint8_t stream) {
    codec_ = codec;
    stream_ = stream;
    capture_ = Capture::Recording;
    searchPending_ = false;
    const uint32_t now = millis();
    lastAudio_ = now;
    keepaliveAt_ = now + 8000;
    decoder_.reset();
    legacyUsed_ = pcmUsed_ = 0;
    dc_ = 0;
    havePrevious_ = false;
    usbMic::startAudio();
    tty().printf("MIC start rate=%u stream=%u\n", codec == adpcm8k ? 8000u : 16000u, stream);
}

void AtvvVoice::finishCapture(bool discard) {
    if (!discard) flushSamples();
    capture_ = Capture::Idle;
    searchPending_ = false;
    legacyUsed_ = pcmUsed_ = 0;
    usbMic::stopAudio(discard);
}

bool AtvvVoice::control(const uint8_t* data, size_t size) {
    if (!write_ || !size) return false;
    switch (data[0]) {
        case 0x00:  // AUDIO_STOP
            if (capture_ != Capture::Idle) tty().println("MIC stop");
            finishCapture(false);
            break;
        case 0x04: {  // AUDIO_START, with optional codec and stream identifier
            const bool unsolicited = capture_ == Capture::Idle && !searchPending_;
            uint8_t codec = preferredCodec_;
            if (size >= 4 && (data[2] == adpcm8k || data[2] == adpcm16k)) codec = data[2];
            beginCapture(codec, size >= 4 ? data[3] : 0);
            return unsolicited;
        }
        case 0x08:  // START_SEARCH; wait for remotes that send AUDIO_START themselves.
            if (capture_ == Capture::Idle) {
                searchPending_ = true;
                searchAt_ = millis() + 150;
            }
            break;
        case 0x0a:  // AUDIO_SYNC: codec, sequence, predictor and step index
            if (capture_ == Capture::Recording && format_ != Format::Legacy && size >= 7 && data[6] <= 88) {
                if (data[1] == adpcm8k || data[1] == adpcm16k) codec_ = data[1];
                decoder_.reset(int16_t(readBe16(data + 4)), data[6]);
            }
            break;
        case 0x0b:
            capabilities(data, size);
            break;
        case 0x0c:
            if (size >= 3) tty().printf("WARN MIC remote error=0x%04X\n", readBe16(data + 1));
            if (capture_ == Capture::Starting) finishCapture(false);
            break;
        default:
            break;
    }
    return false;
}

void AtvvVoice::audio(const uint8_t* bytes, size_t size) {
    if (!size || capture_ == Capture::Idle) return;
    if (capture_ == Capture::Starting) beginCapture(preferredCodec_, 0);
    lastAudio_ = millis();
    ++packets_;
    if (format_ != Format::Legacy) {
        decode(bytes, size);
    } else {
        while (size) {
            const size_t copied = std::min(size, legacySize_ - legacyUsed_);
            memcpy(legacy_ + legacyUsed_, bytes, copied);
            legacyUsed_ += copied;
            bytes += copied;
            size -= copied;
            if (legacyUsed_ != legacySize_) continue;
            legacyUsed_ = 0;
            if (legacy_[5] > 88) {
                ++badFrames_;
                continue;
            }
            const int16_t predictor = int16_t(readBe16(legacy_ + 3));
            decoder_.reset(predictor, legacy_[5]);
            sample(predictor);
            decode(legacy_ + legacyHeader, legacySize_ - legacyHeader);
        }
    }
    flushSamples();
}

void AtvvVoice::decode(const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        sample(decoder_.decode(bytes[i] >> 4));
        sample(decoder_.decode(bytes[i] & 0x0f));
    }
}

void AtvvVoice::sample(int16_t value) {
    // Remove a slowly varying DC offset before sending signed PCM to USB.
    dc_ += (int32_t(value) * 256 - dc_) / 256;
    const int32_t corrected = value - dc_ / 256;
    const int16_t output = int16_t(std::max<int32_t>(-32768, std::min<int32_t>(32767, corrected)));
    auto append = [&](int16_t pcm) {
        pcm_[pcmUsed_++] = pcm;
        if (pcmUsed_ == sizeof(pcm_) / sizeof(pcm_[0])) flushSamples();
    };
    if (codec_ == adpcm8k) append(havePrevious_ ? int16_t((int32_t(previous_) + output) / 2) : output);
    append(output);
    previous_ = output;
    havePrevious_ = true;
}

void AtvvVoice::flushSamples() {
    if (!pcmUsed_) return;
    usbMic::pushAudio(pcm_, pcmUsed_);
    samples_ += pcmUsed_;
    pcmUsed_ = 0;
}

void AtvvVoice::poll() {
    if (searchPending_ && expired(searchAt_)) {
        searchPending_ = false;
        if (!start()) tty().println("WARN MIC open command could not be sent");
    }
    if (capture_ != Capture::Idle && millis() - lastAudio_ > 2500) {
        tty().println("WARN MIC timed out waiting for BLE audio");
        stop();
    }
    if (capture_ == Capture::Recording && format_ == Format::Standard && stream_ == 0 && expired(keepaliveAt_)) {
        const uint8_t command[] = {micExtend, stream_};
        write_(command, sizeof(command));
        keepaliveAt_ = millis() + 8000;
    }
}

void AtvvVoice::status() const {
    const char* mode = !write_ ? "unavailable" : capture_ == Capture::Starting ? "opening" :
                       capture_ == Capture::Recording ? "recording" : "ready";
    tty().printf("MIC state=%s usb_capture=%u packets=%lu pcm_samples=%lu bad_frames=%lu\n",
                 mode, unsigned(usbMic::listening()), static_cast<unsigned long>(packets_),
                 static_cast<unsigned long>(samples_), static_cast<unsigned long>(badFrames_));
}

#endif
