#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "cart.h"
#include "widescreen.h"

CPU cpu;
uint8_t *rom;
uint8_t *cart_ram;
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
    free(cart_ram);
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
    rom = (uint8_t *)calloc(0x200000u, 1);
    if (!rom) return 2;
    test_info.size = 0x100000u;
    test_info.ram_bytes = 0x8000u;
    cart_ram = (uint8_t *)calloc(0x8000u, 1);
    if (!cart_ram) return fail(2, "couldn't allocate synthetic cartridge RAM");

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

    /* Verified GB metatile table: each 16x16 metatile uses four tile IDs. */
    test_info.crc = 0x9C54358Du;
    rom[0x18000u + 0x42u * 4u + 2u] = 0x6B;
    uint8_t metatile_tile = 0;
    if (!wide_wl2_block_tile(0x42, 2, &metatile_tile) || metatile_tile != 0x6B)
        return fail(19, "verified GB metatile lookup failed");
    test_info.crc = 0;
    if (wide_wl2_block_tile(0x42, 2, &metatile_tile))
        return fail(20, "unknown GB ROM revision used fixed metatile data");
    test_info.crc = 0x9C54358Du;

    /* WL2's documented map address increments once per 16px block, with
       256 entries per row; level data is in the linear cartridge-RAM domain. */
    cart_ram[0x2000] = 0x42; /* world block (0,0) -> $A000 */
    cart_ram[0x2001] = 0x43; /* world block (1,0) -> $A001 */
    cart_ram[0x2002] = 0x82; /* high bit is a map flag, not a metatile ID */
    cart_ram[0x2100] = 0x44; /* world block (0,1) -> $A100 */
    cart_ram[0x2010] = 0x45; /* world block (16,0) -> $A010 */
    uint8_t block = 0;
    if (!wide_wl2_block_at(5, 7, &block) || block != 0x42)
        return fail(15, "WL2 world map lookup at block (0,0) failed");
    if (!wide_wl2_block_at(21, 7, &block) || block != 0x43)
        return fail(16, "WL2 world map lookup at block (1,0) failed");
    if (!wide_wl2_block_at(37, 7, &block) || block != 0x02)
        return fail(22, "WL2 world map flag was not normalized to a 7-bit metatile ID");
    if (!wide_wl2_block_at(5, 23, &block) || block != 0x44)
        return fail(17, "WL2 world map lookup at block (0,1) failed");
    if (!wide_wl2_block_at(261, 7, &block) || block != 0x45)
        return fail(18, "WL2 world map lookup at block (16,0) failed");

    rom[0x143] = 0x80;
    if (wide_install_profile(GAME_WARIO_LAND2_GB, 32, 32, 0))
        return fail(10, "GB profile accepted a CGB-compatible cartridge");

    /* Both CGB-enhanced (80) and CGB-only (C0) headers belong to the GBC slot. */
    before = rom_hash();
    if (!wide_install_profile(GAME_WARIO_LAND2_GBC, 32, 32, 0))
        return fail(11, "valid CGB-compatible cartridge header was rejected");
    if (rom_hash() != before)
        return fail(12, "GBC profile unexpectedly modified the ROM image");

    /* The verified GBC release shares the documented metatile table layout. */
    test_info.size = 0x200000u;
    test_info.crc = 0x047BDF80u;
    rom[0x18000u + 0x42u * 4u + 2u] = 0x5B;
    if (!wide_wl2_block_tile(0x42, 2, &metatile_tile) || metatile_tile != 0x5B)
        return fail(21, "verified GBC metatile lookup failed");

    rom[0x143] = 0xC0;
    if (!wide_install_profile(GAME_WARIO_LAND2_GBC, 32, 32, 0))
        return fail(13, "CGB-only cartridge header was rejected");
    rom[0x143] = 0x00;
    if (wide_install_profile(GAME_WARIO_LAND2_GBC, 32, 32, 0))
        return fail(14, "GBC profile accepted a monochrome cartridge");

    free(cart_ram);
    free(rom);
    puts("Wario Land II GB/GBC widescreen profile checks passed");
    return 0;
}
