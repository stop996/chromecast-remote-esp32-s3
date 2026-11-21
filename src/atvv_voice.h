#pragma once

#if defined(CHROMECAST_USB_MIC)

#include <stddef.h>
#include <stdint.h>

#include "ima_adpcm.h"

class AtvvVoice {
public:
    using Write = bool (*)(const uint8_t*, size_t);

    void connect(Write command);
    void disconnect();
    bool control(const uint8_t* data, size_t size);
    void audio(const uint8_t* data, size_t size);
    void poll();
    bool start();
    bool stop();
    bool active() const { return capture_ != Capture::Idle || searchPending_; }
    void status() const;

private:
    enum class Format { Chromecast, Standard, Legacy };
    enum class Capture { Idle, Starting, Recording };

    void capabilities(const uint8_t* data, size_t size);
    void beginCapture(uint8_t codec, uint8_t stream);
    void finishCapture(bool discard);
    void decode(const uint8_t* bytes, size_t size);
    void sample(int16_t sample);
    void flushSamples();

    Write write_ = nullptr;
    Format format_ = Format::Chromecast;
    Capture capture_ = Capture::Idle;
    uint8_t preferredCodec_ = 2;
    uint8_t codec_ = 2;
    uint8_t stream_ = 0;
    bool searchPending_ = false;
    uint32_t searchAt_ = 0;
    uint32_t lastAudio_ = 0;
    uint32_t keepaliveAt_ = 0;
    uint32_t packets_ = 0;
    uint32_t samples_ = 0;
    uint32_t badFrames_ = 0;
    uint8_t legacy_[512];
    size_t legacySize_ = 0;
    size_t legacyUsed_ = 0;
    ImaAdpcm decoder_;
    int32_t dc_ = 0;
    int16_t previous_ = 0;
    bool havePrevious_ = false;
    int16_t pcm_[128];
    size_t pcmUsed_ = 0;
};

#endif
