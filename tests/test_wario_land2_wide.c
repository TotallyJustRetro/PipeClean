#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "cart.h"
#include "widescreen.h"

CPU cpu;
uint8_t *rom;
const uint8_t *cart_lo, *cart_hi;
uint64_t total_cycles;
int frame_count;

static CartInfo test_info;
const CartInfo *cart_info(void) { return &test_info; }

uint8_t rd8(uint16_t a) { (void)a; return 0; }
void hw_tick(int tcycles) { total_cycles += (uint64_t)tcycles; }

static int fail(int code, const char *msg)
{
    fprintf(stderr, "Wario Land II widescreen test failed: %s\n", msg);
    free(rom);
    return code;
}

static uint32_t rom_hash(void)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < 0x100000u; ++i) {
        hash ^= rom[i];
        hash *= 16777619u;
    }
    return hash;
}

int main(void)
{
    int l, r;
    rom = (uint8_t *)calloc(0x100000u, 1);
    if (!rom) return 2;
    test_info.size = 0x100000u;

    wide_dims_profile(GAME_WARIO_LAND2_GB, 0, 0, &l, &r);
    if (l != 0 || r != 0) return fail(3, "GB 0% should keep the native viewport");
    wide_dims_profile(GAME_WARIO_LAND2_GB, 50, 0, &l, &r);
    if (l != 16 || r != 16) return fail(4, "GB 50% dimensions changed");
    wide_dims_profile(GAME_WARIO_LAND2_GB, 100, 0, &l, &r);
    if (l != 32 || r != 32) return fail(5, "GB maximum dimensions changed");

    wide_dims_profile(GAME_WARIO_LAND2_GBC, 0, 0, &l, &r);
    if (l != 0 || r != 0) return fail(6, "GBC 0% should keep the native viewport");
    wide_dims_profile(GAME_WARIO_LAND2_GBC, 100, 0, &l, &r);
    if (l != 32 || r != 32) return fail(7, "GBC maximum dimensions changed");

    /* The monochrome profile must not accept a Color cartridge. */
    rom[0x143] = 0x00;
    uint32_t before = rom_hash();
    if (!wide_install_profile(GAME_WARIO_LAND2_GB, 32, 32, 0))
        return fail(8, "valid GB cartridge header was rejected");
    if (rom_hash() != before)
        return fail(9, "GB profile unexpectedly modified the ROM image");
    rom[0x143] = 0x80;
    if (wide_install_profile(GAME_WARIO_LAND2_GB, 32, 32, 0))
        return fail(10, "GB profile accepted a CGB-compatible cartridge");

    /* Both CGB-enhanced (80) and CGB-only (C0) headers belong to the GBC slot. */
    before = rom_hash();
    if (!wide_install_profile(GAME_WARIO_LAND2_GBC, 32, 32, 0))
        return fail(11, "valid CGB-compatible cartridge header was rejected");
    if (rom_hash() != before)
        return fail(12, "GBC profile unexpectedly modified the ROM image");
    rom[0x143] = 0xC0;
    if (!wide_install_profile(GAME_WARIO_LAND2_GBC, 32, 32, 0))
        return fail(13, "CGB-only cartridge header was rejected");
    rom[0x143] = 0x00;
    if (wide_install_profile(GAME_WARIO_LAND2_GBC, 32, 32, 0))
        return fail(14, "GBC profile accepted a monochrome cartridge");

    free(rom);
    puts("Wario Land II GB/GBC widescreen profile checks passed");
    return 0;
}
