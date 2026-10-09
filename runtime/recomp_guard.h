#pragma once
/* Guard the statically recompiled module against being used with another ROM.
 * tools/recomp.py bakes one cartridge's instructions into the executable. */
#include "cart.h"
#include "game_info.h"
#include <string.h>

static inline int recomp_rom_matches_build(const uint8_t *img, size_t n)
{
    CartInfo ci;
    if (!img || !cart_parse(img, n, &ci)) return 0;
    return ci.crc == ROM_CRC32 && strcmp(ci.title, ROM_TITLE) == 0;
}
