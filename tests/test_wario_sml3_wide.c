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
    fprintf(stderr, "Wario Land widescreen test failed: %s\n", msg);
    free(rom);
    return code;
}

static void seed_wario_fov(void)
{
    rom[0x3218] = 0xC6; rom[0x3219] = 0x10;
    rom[0x321A] = 0xFE; rom[0x321B] = 0xD0;
}

static void seed_dx_sites(void)
{
    rom[0x321C] = 0xD0;
    rom[0x31FC] = 0x01; rom[0x31FD] = 0xD0; rom[0x31FE] = 0x00;
    rom[0x8476] = 0xD6; rom[0x8477] = 0x07;
    rom[0x8485] = 0x1E; rom[0x8486] = 0x0E;
    rom[0x849F] = 0xC6; rom[0x84A0] = 0x06;
    rom[0x84BF] = 0xD6; rom[0x84C0] = 0x07;
}

int main(void)
{
    int l, r;
    rom = (uint8_t *)calloc(0x80000u, 1);
    if (!rom) return 2;
    test_info.size = 0x80000u;

    wide_dims_profile(GAME_WARIO_SML3, 0, 0, &l, &r);
    if (l || r) return fail(3, "0% should preserve the original viewport");
    wide_dims_profile(GAME_WARIO_SML3, 100, 0, &l, &r);
    if (l != 33 || r != 33) return fail(4, "clean ROM should expose 33px per side");
    wide_dims_profile(GAME_WARIO_SML3, 100, 1, &l, &r);
    if (l != 30 || r != 30) return fail(5, "DX profile should expose 30px per side");
    if (!wide_is_wario_dx_profile("Wario Land DX v1.2", "patches/wl-sm3-dx.ips") ||
        wide_is_wario_dx_profile("Wario Land", "patches/standard.ips"))
        return fail(6, "DX profile detection is wrong");

    memset(rom, 0, 0x80000u);
    seed_wario_fov();
    if (!wide_install_profile(GAME_WARIO_SML3, 33, 33, 0))
        return fail(7, "clean Wario Land signature was rejected");
    if (rom[0x3218] != 0xC6 || rom[0x3219] != 0x21 ||
        rom[0x321A] != 0xFE || rom[0x321B] != 0xE2)
        return fail(8, "clean-game culling bounds were not widened");
    if (!wide_install_profile(GAME_WARIO_SML3, 33, 33, 0))
        return fail(9, "clean-game patch should be safe to recheck");

    memset(rom, 0, 0x80000u);
    seed_wario_fov();
    seed_dx_sites();
    if (!wide_install_profile(GAME_WARIO_SML3, 30, 30, 1))
        return fail(10, "DX Wario Land signatures were rejected");
    if (rom[0x3219] != 0x1E || rom[0x321B] != 0xED ||
        rom[0x321C] != 0x00 || rom[0x31FD] != 0xD1 ||
        rom[0x8477] != 0x08 || rom[0x8486] != 0x10 ||
        rom[0x84A0] != 0x08 || rom[0x84C0] != 0x08)
        return fail(11, "DX-specific view and activation sites were not patched");
    if (!wide_install_profile(GAME_WARIO_SML3, 30, 30, 1))
        return fail(12, "DX patch should be safe to recheck");

    memset(rom, 0, 0x80000u);
    seed_wario_fov();
    seed_dx_sites();
    rom[0x8486] = 0x22;
    if (wide_install_profile(GAME_WARIO_SML3, 30, 30, 1))
        return fail(13, "an incompatible DX build was accepted");
    if (rom[0x3219] != 0x10 || rom[0x321B] != 0xD0 ||
        rom[0x321C] != 0xD0 || rom[0x8477] != 0x07)
        return fail(14, "an incompatible ROM was partially changed");

    free(rom);
    puts("Wario Land SML3 widescreen: PASS");
    return 0;
}
