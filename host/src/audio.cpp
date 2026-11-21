#include "qilin/host/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace qilin::host {

namespace {
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "fatal: %s: %s\n", what, SDL_GetError());
    std::fflush(nullptr);
    std::_Exit(70);
}
size_t frames_of(double ms, unsigned rate) { return static_cast<size_t>(ms * double(rate) / 1000.0); }
}  // namespace

AudioOut::AudioOut() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) die("SDL audio can't start");
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    rate_ = 32000;
    // The device asks for sound as it needs it (feed): nothing waits inside SDL.
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, [](void* self, SDL_AudioStream* stream, int additional, int) {
        static_cast<AudioOut*>(self)->feed(stream, additional);
    }, this);
    if (!stream_) die("no sound output device");
    // A device that takes its sound in large pieces needs more queued than one that takes 10 ms at a time.
    SDL_AudioSpec device{};
    int piece = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &piece) && device.freq > 0 && piece > 0)
        device_period_ms_ = double(piece) * 1000.0 / double(device.freq);
    target_ms_ = std::max(kTargetMs, 2.0 * device_period_ms_ + 10.0);
    smoothed_ms_ = target_ms_;
    queue_.set_limits(frames_of(target_ms_, rate_), frames_of(2.0 * target_ms_, rate_));
    if (!SDL_ResumeAudioStreamDevice(stream_)) die("the sound output device won't start");
}

AudioOut::~AudioOut() {
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

// The device's thread: SDL needs `bytes` more of the stream's own format.
void AudioOut::feed(SDL_AudioStream* stream, int bytes) {
    if (bytes <= 0) return;
    const size_t frames = (static_cast<size_t>(bytes) + 3) / 4;
    feeding_.resize(frames * 2);
    size_t n;
    bool ran_dry;
    {
        std::lock_guard l(lock_);
        n = queue_.pull(feeding_.data(), frames, ran_dry);
    }
    if (ran_dry) empty_.fetch_add(1, std::memory_order_relaxed);
    if (n) SDL_PutAudioStreamData(stream, feeding_.data(), static_cast<int>(n * 4));  // (short of it, the device plays silence)
}

void AudioOut::push(const int16_t* lr, size_t frames, unsigned rate) {
    size_t before, dropped;
    {
        std::lock_guard l(lock_);
        if (rate != rate_) {  // what is queued was made for the old rate: it goes
            const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
            if (!SDL_SetAudioStreamFormat(stream_, &spec, nullptr)) die("the sound rate can't change");
            rate_ = rate;
            queue_.clear();
            queue_.set_limits(frames_of(target_ms_, rate_), frames_of(2.0 * target_ms_, rate_));
        }
        before = queue_.size();
        dropped = queue_.push(lr, frames);
    }
    if (dropped) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        dropped_frames_.fetch_add(dropped, std::memory_order_relaxed);
        smoothed_ms_ = target_ms_;
    }
    const double ms = double(before) * 1000.0 / double(rate);
    // Smoothed over about 50 pushes (a quarter second at 5 ms each): the drift is slow.
    smoothed_ms_ += (ms - smoothed_ms_) * 0.02;
    const double ratio = audio_rate_ratio(smoothed_ms_, target_ms_);
    if (std::fabs(ratio - ratio_) >= 0.0001) {  // SDL rebuilds its resampler: not for every push
        if (!SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio))) die("the sound speed can't change");
        ratio_ = ratio;
        ratio_ppm_.store(static_cast<uint32_t>(std::lround(ratio * 1e6)), std::memory_order_relaxed);
    }
    const auto us = static_cast<uint32_t>(ms * 1000.0);
    sum_us_.fetch_add(us, std::memory_order_relaxed);
    pushes_.fetch_add(1, std::memory_order_relaxed);
    uint32_t m = max_us_.load(std::memory_order_relaxed);
    while (us > m && !max_us_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
}

AudioOut::Stats AudioOut::take_stats() {
    Stats s;
    s.pushes = pushes_.exchange(0, std::memory_order_relaxed);
    const uint64_t sum = sum_us_.exchange(0, std::memory_order_relaxed);
    s.average_ms = s.pushes ? double(sum) / double(s.pushes) / 1000.0 : 0;
    s.max_ms = max_us_.exchange(0, std::memory_order_relaxed) / 1000.0;
    s.empty = empty_.exchange(0, std::memory_order_relaxed);
    s.dropped = dropped_.exchange(0, std::memory_order_relaxed);
    unsigned rate;
    {
        std::lock_guard l(lock_);
        rate = rate_;
    }
    s.dropped_ms = double(dropped_frames_.exchange(0, std::memory_order_relaxed)) * 1000.0 / double(rate);
    s.ratio = ratio_ppm_.load(std::memory_order_relaxed) / 1e6;
    return s;
}

}  // namespace qilin::host
