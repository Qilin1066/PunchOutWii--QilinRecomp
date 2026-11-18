// Sound: the DSP and the audio interface (dsp.cpp), and the DSP's AX
// microcode run at a high level (ax.cpp). The game's AX, AI and DSP libraries
// are lifted and drive this hardware unchanged.
#pragma once

#include <cstddef>
#include <cstdint>

#include "qilin/rt/system.hpp"

namespace punchout {

// Map the DSP (0xCC005000) and AI (0xCD006C00) registers; init_hardware calls it.
void init_audio();

// The OS's interrupt mask changed: set the DSP control register's enables
// for interrupts 5, 6 and 7 (AI DMA, ARAM, DSP) to match, as the OS does.
void dsp_interrupt_enables(uint32_t os_mask);

// Where the audio DMA's samples go: interleaved left, right, host byte
// order, at `rate` Hz. Called on a guest thread. Without a sink, sound is
// computed and discarded (headless).
using AudioSink = void (*)(const int16_t* left_right, size_t frames, unsigned rate, void* user);
void set_audio_sink(AudioSink sink, void* user);

struct AudioStats {
    uint64_t dma_periods = 0, frames = 0, command_lists = 0, voices = 0, mails_in = 0, mails_out = 0;
    uint32_t ucode_crc = 0;
};
const AudioStats& audio_stats();

// Between dsp.cpp and ax.cpp.
namespace dsp {
// The DSP sends a mail to the CPU; with `interrupt`, the CPU's DSP interrupt
// is raised when the mail before it has been read (or now, if none waits).
void push_mail(uint32_t mail, bool interrupt);
// Load the ROM's boot microcode again (a task asked for a DSP reset).
void reset_to_rom();
// Guest memory at a physical address (MEM1 0x0..., MEM2 0x1...; segment bits ignored).
uint8_t* physical(qilin::rt::Cpu* c, uint32_t address, uint32_t size);
AudioStats& stats();
}  // namespace dsp

namespace ax {
// True if `crc` (the microcode's hash) is an AX version this knows.
bool known(uint32_t crc);
// The microcode has booted: it greets the CPU.
void start(uint32_t crc);
// A mail from the CPU while AX runs.
void mail(qilin::rt::Cpu& c, uint32_t mail);
}  // namespace ax

}  // namespace punchout
