// Textures from guest memory (texture.cpp): GX's formats decoded to RGBA8,
// cached by what they were decoded from, and the textures EFB copies make.
#pragma once

#include <cstdint>

#include "gx.hpp"

namespace punchout::gx {

// BP 0x65: copy `bytes` of palette from guest memory into TMEM.
void load_tlut(uint32_t source, uint32_t tmem_address, uint32_t bytes);

// The texture texture map `map` names in the BP registers `bp`: its id,
// decoded into `frame` if it's new or its memory changed. Width and height
// out. -1 if the map has no address.
int32_t texture_for(const uint32_t* bp, unsigned map, Frame& frame, uint32_t& width, uint32_t& height);

// An EFB copy to a texture at `address`: the id it becomes. Later reads of
// a texture of that size at that address get the copy, until the game
// writes that memory.
int32_t texture_from_copy(uint32_t address, uint32_t width, uint32_t height, Frame& frame);

// Once per frame: forget what hasn't been used for a while.
void textures_end_frame(Frame& frame);

// GX's texture formats and their sizes in memory.
uint32_t texture_bytes(unsigned format, uint32_t width, uint32_t height);
// Decode one level (tests): RGBA8 out, width * height * 4 bytes.
void decode_texture(unsigned format, const uint8_t* src, uint32_t width, uint32_t height, const uint8_t* tlut,
                    unsigned tlut_format, uint8_t* out);

}  // namespace punchout::gx
