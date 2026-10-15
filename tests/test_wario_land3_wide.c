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

static int fail(int code, const char *message)
{
    fprintf(stderr, "Wario Land 3 widescreen test failed: %s\n", message);
    free(rom);
    return code;
}

static uint32_t rom_hash(void)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < test_info.size; ++i) {
        hash ^= rom[i];
        hash *= 16777619u;
    }
    return hash;
}

int main(void)
{
    int left = 0, right = 0;
    rom = (uint8_t *)calloc(0x100000u, 1);
    if (!rom) return 2;
    test_info.size = 0x80000u;
    test_info.cgb_flag = 0xC0u;
    rom[0x143] = 0xC0u;

    wide_dims_profile(GAME_WARIO_LAND3_GBC, 0, 0, &left, &right);
    if (left || right) return fail(3, "0% should preserve the native viewport");
    wide_dims_profile(GAME_WARIO_LAND3_GBC, 50, 0, &left, &right);
    if (left != 16 || right != 16) return fail(4, "50% should expose 16px on each side");
    wide_dims_profile(GAME_WARIO_LAND3_GBC, 100, 0, &left, &right);
    if (left != 32 || right != 32) return fail(5, "100% should expose 32px on each side");

    uint32_t before = rom_hash();
    if (!wide_install_profile(GAME_WARIO_LAND3_GBC, 32, 32, 0))
        return fail(6, "CGB Wario Land 3 cartridge should accept the diagnostic profile");
    if (rom_hash() != before)
        return fail(7, "installing widescreen unexpectedly modified the ROM");

    rom[0x143] = 0;
    if (wide_install_profile(GAME_WARIO_LAND3_GBC, 32, 32, 0))
        return fail(8, "monochrome cartridge should not activate the Wario Land 3 profile");

    free(rom);
    puts("Wario Land 3 widescreen profile checks passed");
    return 0;
}
