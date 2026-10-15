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
static uint8_t test_wram[0x8000];
const uint8_t *cart_lo, *cart_hi;
uint64_t total_cycles;
int frame_count;

static CartInfo test_info;
const CartInfo *cart_info(void) { return &test_info; }
uint8_t rd8(uint16_t a)
{
    if (a >= 0xC000u && a < 0xE000u) return test_wram[a - 0xC000u];
    return 0;
}
uint8_t gb_wram_peek(size_t offset)
{
    return offset < sizeof test_wram ? test_wram[offset] : 0xFFu;
}
void hw_tick(int tcycles) { total_cycles += (uint64_t)tcycles; }

static int fail(int code, const char *message)
{
    fprintf(stderr, "Wario Land 3 widescreen test failed: %s\n", message);
    free(rom);
    free(cart_ram);
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

static void put_bounds_signature(size_t off, uint8_t object_adjust)
{
    const uint8_t code[40] = {
        0x21, 0x30, 0xC2, 0x2A, 0x6E, 0x67, 0x7B, 0xC6, object_adjust, 0x5F,
        0x30, 0x04, 0x14, 0x28, 0x02, 0x78, 0xBC, 0x38, 0x03, 0x20,
        0x04, 0x79, 0xBD, 0x38, 0x02, 0x28, 0x02, 0x7D, 0xC6, 0xA0,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(rom + off, code, sizeof code);
}

static void put_right_spawn_signature(size_t off)
{
    const uint8_t code[22] = {
        0x21, 0x31, 0xC2, 0x3A, 0xE6, 0xF8, 0x5F, 0x66, 0x6B, 0x11, 0xB0,
        0x00, 0x19, 0x7C, 0xFE, 0x0A, 0x38, 0x02, 0x21, 0xF8, 0x09, 0x7C
    };
    memcpy(rom + off, code, sizeof code);
}

static void put_left_spawn_signature(size_t off)
{
    const uint8_t code[20] = {
        0x21, 0x31, 0xC2, 0x3A, 0xE6, 0xF8, 0x5F, 0x66, 0x6B, 0x11,
        0xF0, 0xFF, 0x19, 0xCB, 0x7C, 0x28, 0x02, 0x21, 0x00, 0x00
    };
    memcpy(rom + off, code, sizeof code);
}

static uint16_t pc_for_rom_offset(size_t off)
{
    return off < 0x4000u ? (uint16_t)off
                         : (uint16_t)(0x4000u + (off & 0x3FFFu));
}

int main(void)
{
    const size_t check_visible = 0x10000u;
    const size_t check_despawn = 0x10100u;
    const size_t spawn_right = 0x10200u;
    const size_t spawn_left = 0x10300u;
    const size_t spawn_left_vertical = 0x10340u;
    const size_t spawn_scan_loop = 0x10500u;
    int left = 0, right = 0;

    rom = (uint8_t *)calloc(0x100000u, 1);
    if (!rom) return 2;
    test_info.size = 0x80000u;
    test_info.ram_bytes = 0x8000u;
    test_info.cgb_flag = 0xC0u;
    cart_ram = (uint8_t *)calloc(test_info.ram_bytes, 1);
    if (!cart_ram) return fail(15, "test cart RAM allocation failed");
    rom[0x143] = 0xC0u;
    cart_lo = rom;
    cart_hi = rom + 0x10000u;

    /* Construct representative copies of the verified instruction shapes.
     * The profile scanner must discover all of them without patching ROM. */
    put_bounds_signature(check_visible, 0x18u);
    put_bounds_signature(check_despawn, 0x6Fu);
    put_right_spawn_signature(spawn_right);
    put_left_spawn_signature(spawn_left);
    put_left_spawn_signature(spawn_left_vertical);
    rom[spawn_scan_loop + 0] = 0x1Eu; /* LD E,$0d */
    rom[spawn_scan_loop + 1] = 0x0Du;
    rom[spawn_scan_loop + 2] = 0xCBu; /* BIT 7,[HL] */
    rom[spawn_scan_loop + 3] = 0x7Eu;
    rom[spawn_scan_loop + 4] = 0x28u; /* JR Z, ... */
    /* Room-table load signature: LD DE,$C800; LD BC,$0200; CALL. */
    rom[0x10600] = 0x11u; rom[0x10601] = 0x00u; rom[0x10602] = 0xC8u;
    rom[0x10603] = 0x01u; rom[0x10604] = 0x00u; rom[0x10605] = 0x02u;
    rom[0x10606] = 0xCDu; rom[0x10607] = 0x34u; rom[0x10608] = 0x12u;
    /* Wario Land 3 level map: SRAM bank 1, row 1, column 2. */
    cart_ram[0x2000u + 0x100u + 2u] = 0x85u;
    test_wram[0x800u + 5u * 4u + 2u] = 0x57u; /* block 5, bottom-left */
    test_wram[0x3300u + 5u * 4u + 2u] = 0xE7u; /* CGB palette + flags */
    test_wram[0x22Eu] = 0x00u; test_wram[0x22Fu] = 0x10u; /* camera Y = 16 */
    test_wram[0x230u] = 0x00u; test_wram[0x231u] = 0x20u; /* camera X = 32 */

    wide_dims_profile(GAME_WARIO_LAND3_GBC, 0, 0, &left, &right);
    if (left || right) return fail(3, "0% should preserve the native viewport");
    wide_dims_profile(GAME_WARIO_LAND3_GBC, 50, 0, &left, &right);
    if (left != 16 || right != 16) return fail(4, "50% should expose 16px on each side");
    wide_dims_profile(GAME_WARIO_LAND3_GBC, 100, 0, &left, &right);
    if (left != 32 || right != 32) return fail(5, "100% should expose 32px on each side");

    uint32_t before = rom_hash();
    if (!wide_install_profile(GAME_WARIO_LAND3_GBC, 32, 32, 0))
        return fail(6, "CGB Wario Land 3 cartridge should accept the widescreen profile");
    if (rom_hash() != before)
        return fail(7, "installing widescreen unexpectedly modified the ROM");

    uint8_t decoded = 0;
    if (wide_wl3_block_at(32, 16, &decoded) != 1 || decoded != 5u)
        return fail(15, "level SRAM map should return the seven-bit block ID");
    if (wide_wl3_block_tile(5, 2, &decoded) != 1 || decoded != 0x57u)
        return fail(16, "runtime room graphics table should supply the block tile");
    if (wide_wl3_block_attr(5, 2, &decoded) != 1 || decoded != 0xE7u)
        return fail(17, "bank-3 room attributes should supply the palette flags");
    int camera_x = 0, camera_y = 0;
    if (!wide_wl3_camera(&camera_x, &camera_y) || camera_x != 32 || camera_y != 16)
        return fail(18, "camera words should be detected from the live object code");
    if (wide_wl3_block_at(-1, 16, &decoded) != -1)
        return fail(19, "negative map coordinates should be treated as outside the level");

    /* The left bound is shifted by the selected extension, while the current
     * LD A,E instruction still runs normally afterwards. */
    cpu.pc = (uint16_t)(pc_for_rom_offset(check_visible) + 6u);
    cpu.h = 0x12u; cpu.l = 0x34u;
    if (wide_intercept_wl3(0x7Bu) != 0 ||
        ((unsigned)cpu.h << 8 | cpu.l) != 0x1214u)
        return fail(8, "visible-object left bound should move left by 32px");

    /* ADD A,$a0 is extended to the actual wider camera window. */
    cpu.pc = (uint16_t)(pc_for_rom_offset(check_visible) + 28u);
    cpu.pc = (uint16_t)(pc_for_rom_offset(check_visible) + 28u);
    cpu.a = 0x10u; cpu.f = 0x40u; total_cycles = 0;
    if (wide_intercept_wl3(0xC6u) != 1 || cpu.a != 0xF0u ||
        cpu.pc != (uint16_t)(pc_for_rom_offset(check_visible) + 30u) ||
        total_cycles != 8u)
        return fail(9, "object width check should use 160+left+right pixels");

    /* The second copy controls object lifetime, so it must get the same
     * widened bounds; otherwise objects vanish as soon as they leave 160px. */
    cpu.pc = (uint16_t)(pc_for_rom_offset(check_despawn) + 6u);
    cpu.h = 0x20u; cpu.l = 0x04u;
    if (wide_intercept_wl3(0x7Bu) != 0 ||
        ((unsigned)cpu.h << 8 | cpu.l) != 0x1FE4u)
        return fail(10, "object-despawn left bound should move left by 32px");

    /* The leading edge used when scrolling right moves out by the extra
     * right margin plus the game's original 16px pre-spawn lead. */
    cpu.pc = (uint16_t)(pc_for_rom_offset(spawn_right) + 12u);
    cpu.h = 0x02u; cpu.l = 0x00u; cpu.f = 0x80u; total_cycles = 0;
    if (wide_intercept_wl3(0x19u) != 1 ||
        ((unsigned)cpu.h << 8 | cpu.l) != 0x02D0u ||
        !(cpu.f & 0x80u) || total_cycles != 8u)
        return fail(11, "right-edge object spawning should extend by 32px");

    cpu.pc = (uint16_t)(pc_for_rom_offset(spawn_left_vertical) + 12u);
    cpu.h = 0x05u; cpu.l = 0x00u; cpu.f = 0x80u; total_cycles = 0;
    if (wide_intercept_wl3(0x19u) != 1 ||
        ((unsigned)cpu.h << 8 | cpu.l) != 0x04D0u ||
        total_cycles != 8u)
        return fail(12, "left-edge object spawning should extend by 32px");

    /* Vertical-edge scans traverse horizontally, so include the additional
     * left/right block columns rather than stopping at the old 13-entry span. */
    cpu.pc = pc_for_rom_offset(spawn_scan_loop);
    cpu.e = 0;
    total_cycles = 0;
    if (wide_intercept_wl3(0x1Eu) != 1 || cpu.e != 17u ||
        cpu.pc != (uint16_t)(pc_for_rom_offset(spawn_scan_loop) + 2u) ||
        total_cycles != 8u)
        return fail(13, "horizontal edge scan should include widened block columns");

    rom[0x143] = 0;
    if (wide_install_profile(GAME_WARIO_LAND3_GBC, 32, 32, 0))
        return fail(14, "monochrome cartridge should not activate the Wario Land 3 profile");

    free(rom);
    free(cart_ram);
    puts("Wario Land 3 widescreen profile, map, room table and object-bound checks passed");
    return 0;
}
