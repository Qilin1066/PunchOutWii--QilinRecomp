// The Wii Remote's speaker, heard through the PC's sound.
//
// The game mixes each remote's speaker sound in AX (6,000 samples a second),
// encodes it 40 samples at a time with the SDK's encoder (WENC) and sends
// the 20 bytes to the remote (WPADSendStreamData), whose sound chip decodes
// them: Yamaha's 4-bit ADPCM, the high half of each byte first. Here the
// remote's side: the decoder, and a short queue per remote that the audio
// DMA's buffers draw from (dsp.cpp), resampled to their rate and added to
// both channels.
//
// The decoder is checked against the game's own encoder (`punchout
// --check-speaker`, main.cpp): after every block the encoder's own idea of
// the decoded signal and step must be exactly the decoder's.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace punchout {

// Yamaha 4-bit ADPCM: each code is a sign and three bits of magnitude against
// a step that grows after large codes and shrinks after small ones.
struct SpeakerDecoder {
    int32_t signal = 0;
    int32_t step = 127;

    void reset() { signal = 0, step = 127; }
    int16_t code(unsigned n) {
        static constexpr int32_t kScale[8] = {230, 230, 230, 230, 307, 409, 512, 614};  // over 256
        // The step's eighth, plus a whole, a half and a quarter of it for the code's three bits
        // (each part rounded down on its own, which is not quite step * (2n + 1) / 8).
        const int32_t delta = (step >> 3) + (n & 4 ? step : 0) + (n & 2 ? step >> 1 : 0) + (n & 1 ? step >> 2 : 0);
        signal = std::clamp(n & 8 ? signal - delta : signal + delta, -32768, 32767);
        step = std::clamp(step * kScale[n & 7] >> 8, 127, 24576);
        return static_cast<int16_t>(signal);
    }
    // `bytes` of codes, two samples each, into `out`.
    void decode(const uint8_t* in, size_t bytes, int16_t* out) {
        for (size_t i = 0; i < bytes; ++i) {
            out[2 * i] = code(in[i] >> 4);
            out[2 * i + 1] = code(in[i] & 15);
        }
    }
};

// One remote's speaker: what it has been sent and not yet played.
class Speaker {
public:
    static constexpr unsigned kRate = 6000;
    // What waits is kept between one and four blocks (7 to 27 ms): the game sends a block every
    // 6.7 ms by its own alarm, and the DMA's buffers take them at their own pace.
    static constexpr size_t kBlock = 40, kMost = 4 * kBlock;

    void reset() { decoder_.reset(), queue_.clear(), at_ = 0, last_ = 0, playing_ = false; }
    void set_volume(unsigned v) { volume_ = std::min(v, 127u); }  // the remote's own, 0 to 127
    void set_muted(bool m) { muted_ = m; }

    // A block from the game (WPADSendStreamData).
    void send(const uint8_t* bytes, size_t n) {
        ++blocks_, loud_ += std::any_of(bytes, bytes + n, [](uint8_t b) { return b != 0x00 && b != 0x88 && b != 0x80 && b != 0x08; });
        const size_t had = queue_.size();
        queue_.resize(had + 2 * n);
        decoder_.decode(bytes, n, queue_.data() + had);
        if (queue_.size() > kMost) queue_.erase(queue_.begin(), queue_.end() - static_cast<std::ptrdiff_t>(kMost));  // (the game ran ahead: the oldest goes)
    }

    // Add `frames` of sound at `rate` to interleaved stereo, saturating. Linear between samples
    // (the remote's own speaker is far cruder than that).
    void mix(int16_t* left_right, size_t frames, unsigned rate, bool on) {
        if (!playing_) {
            if (queue_.size() < 2 * kBlock) return;  // a block in hand before it starts, so the next arrives in time
            playing_ = true;
        }
        const uint32_t step = static_cast<uint32_t>((uint64_t{kRate} << 16) / rate);  // 16.16 samples a frame
        for (size_t i = 0; i < frames; ++i) {
            if (queue_.empty()) {  // ran out: silence until there's a cushion again
                playing_ = false;
                last_ = 0;
                return;
            }
            const int32_t a = last_, b = queue_.front();
            const int32_t s = a + (((b - a) * static_cast<int32_t>(at_ >> 4)) >> 12);
            if (on && !muted_) {
                const int32_t v = s * static_cast<int32_t>(volume_) / 127;
                left_right[2 * i] = static_cast<int16_t>(std::clamp(left_right[2 * i] + v, -32768, 32767));
                left_right[2 * i + 1] = static_cast<int16_t>(std::clamp(left_right[2 * i + 1] + v, -32768, 32767));
            }
            at_ += step;
            while (at_ >= 0x10000 && !queue_.empty()) {
                at_ -= 0x10000;
                last_ = queue_.front();
                queue_.erase(queue_.begin());
            }
        }
    }

    size_t queued() const { return queue_.size(); }
    // Blocks sent since the last call, and how many of them weren't silence (for the log).
    void take_counts(uint64_t& blocks, uint64_t& loud) { blocks = blocks_, loud = loud_, blocks_ = loud_ = 0; }

private:
    SpeakerDecoder decoder_;
    std::vector<int16_t> queue_;
    uint32_t at_ = 0;      // 16.16 of the way from last_ to the queue's front
    int16_t last_ = 0;
    unsigned volume_ = 64;
    uint64_t blocks_ = 0, loud_ = 0;
    bool muted_ = false, playing_ = false;
};

// The remotes' speakers (wpad.cpp sends; dsp.cpp mixes them into each buffer the audio DMA
// plays). All on guest threads, which run one at a time.
inline Speaker g_speakers[2];
inline std::atomic<bool> g_speakers_on{true};  // the settings': heard at all (the main thread sets it)

}  // namespace punchout
