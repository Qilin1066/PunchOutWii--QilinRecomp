// Sound out: interleaved 16-bit stereo through an SDL audio stream on the
// default playback device. push() may be called from any thread.
#pragma once

#include <cstddef>
#include <cstdint>

struct SDL_AudioStream;

namespace qilin::host {

class AudioOut {
public:
    AudioOut();  // fails loudly if there's no playback device
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Queue `frames` left/right pairs at `rate` Hz. More than 150 ms queued
    // (the guest ran ahead of the device's clock) is dropped to keep latency down.
    void push(const int16_t* left_right, size_t frames, unsigned rate);

private:
    SDL_AudioStream* stream_ = nullptr;
    unsigned rate_ = 0;
};

}  // namespace qilin::host
