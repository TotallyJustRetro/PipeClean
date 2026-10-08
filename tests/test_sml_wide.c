#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "widescreen.h"
#include "cart.h"

CPU cpu;
uint8_t *rom;
const uint8_t *cart_lo, *cart_hi;
uint64_t total_cycles;

void hw_tick(int tcycles)
{
    total_cycles += (uint64_t)tcycles;
}

static void seed_sml_rom(void)
{
    static const uint8_t o1[] = {0xFA, 0xAB, 0xC0};
    static const uint8_t o2[] = {0xC6, 0xD0};
    static const uint8_t o3[] = {0xF0, 0xC3, 0xFE, 0xE0, 0x38, 0x0A};

    memset(rom, 0, 0x8000);
    memcpy(&rom[0x24A5], o1, sizeof o1);
    memcpy(&rom[0x24BF], o2, sizeof o2);
    memcpy(&rom[0x2584], o3, sizeof o3);
}

static int fail(int code, const char *msg)
{
    fprintf(stderr, "SML widescreen test failed: %s\n", msg);
    free(rom);
    return code;
}

int main(void)
{
    int l, r;

    rom = (uint8_t *)calloc(0x8000, 1);
    if (!rom) return 2;

    /* 0% must leave the normal 160-pixel presentation untouched. */
    wide_dims(GAME_SML, 0, &l, &r);
    if (l != 0 || r != 0) return fail(3, "zero percent produced a non-zero extension");

    /* The full profile is 19 px left + 56 px right = 235 px total. */
    wide_dims(GAME_SML, 100, &l, &r);
    if (l != 19 || r != 56) return fail(4, "100% profile dimensions changed");

    seed_sml_rom();
    if (!wide_install(GAME_SML, l, r)) return fail(5, "expected SML signature was rejected");

    if (rom[0x24A5] != 0xCD || rom[0x24A6] != 0xE4 || rom[0x24A7] != 0x3F)
        return fail(6, "spawn routine was not redirected to the trampoline");
    if (rom[0x24C0] != 0xF0)
        return fail(7, "spawn X offset was not expanded");
    if (rom[0x2584] != 0xCD || rom[0x2585] != 0xEA || rom[0x2586] != 0x3F)
        return fail(8, "despawn routine was not redirected to the trampoline");
    if (rom[0x3FE4] != 0xFA || rom[0x3FE5] != 0xAB || rom[0x3FE6] != 0xC0 ||
        rom[0x3FE7] != 0xC6 || rom[0x3FE8] != 0x02 || rom[0x3FE9] != 0xC9)
        return fail(9, "spawn trampoline contents are wrong");
    if (rom[0x3FEA] != 0xF0 || rom[0x3FEB] != 0xC3 || rom[0x3FEC] != 0xFE ||
        rom[0x3FED] != 0xD3 || rom[0x3FEE] != 0xD8 || rom[0x3FEF] != 0xFE ||
        rom[0x3FF0] != 0xEC || rom[0x3FF1] != 0x3F || rom[0x3FF2] != 0xC9)
        return fail(10, "despawn trampoline contents are wrong");

    /* A mismatched ROM must be rejected without partially patching it. */
    seed_sml_rom();
    rom[0x24A5] = 0x00;
    if (wide_install(GAME_SML, l, r)) return fail(11, "mismatched ROM was accepted");
    if (rom[0x24A5] != 0x00) return fail(12, "mismatched ROM was modified");

    free(rom);
    puts("SML widescreen: PASS");
    return 0;
}
