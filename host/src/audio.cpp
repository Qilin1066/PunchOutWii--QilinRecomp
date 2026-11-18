#include "qilin/host/audio.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>

namespace qilin::host {

namespace {
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "fatal: %s: %s\n", what, SDL_GetError());
    std::fflush(nullptr);
    std::_Exit(70);
}
}  // namespace

AudioOut::AudioOut() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) die("SDL audio can't start");
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) die("no sound output device");
    rate_ = 32000;
    if (!SDL_ResumeAudioStreamDevice(stream_)) die("the sound output device won't start");
}

AudioOut::~AudioOut() {
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::push(const int16_t* lr, size_t frames, unsigned rate) {
    if (rate != rate_) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
        if (!SDL_SetAudioStreamFormat(stream_, &spec, nullptr)) die("the sound rate can't change");
        rate_ = rate;
    }
    if (SDL_GetAudioStreamQueued(stream_) > static_cast<int>(rate * 4 * 15 / 100)) SDL_ClearAudioStream(stream_);
    if (!SDL_PutAudioStreamData(stream_, lr, static_cast<int>(frames * 4))) die("sound can't be queued");
}

}  // namespace qilin::host
