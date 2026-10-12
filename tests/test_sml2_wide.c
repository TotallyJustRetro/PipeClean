#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "sm83.h"
#include "widescreen.h"
#include "cart.h"

CPU cpu;
uint8_t *rom;
const uint8_t *cart_lo, *cart_hi;
uint64_t total_cycles;
int frame_count;
uint8_t vram[0x2000];
static uint8_t test_level_map[0x3000];
static uint8_t test_block_defs[0x200];
static uint8_t test_hram[0x80];
static uint16_t test_camera_x = 640;
static uint16_t test_world_x = 320;
static uint16_t test_world_y = 32;
static uint8_t test_scx;

uint8_t rd8(uint16_t a)
{
    /* Camera X defaults to 640, camera Y = 72 (native view begins at world Y=0). */
    if (a == 0xFFCA) return (uint8_t)test_camera_x;
    if (a == 0xFFCB) return (uint8_t)(test_camera_x >> 8);
    if (a == 0xA680) return (uint8_t)test_world_x;
    if (a == 0xA681) return (uint8_t)(test_world_x >> 8);
    if (a == 0xA682) return (uint8_t)test_world_y;
    if (a == 0xA683) return (uint8_t)(test_world_y >> 8);
    if (a == 0xFF43) return test_scx;
    if (a == 0xFFC8) return 0x48;
    if (a == 0xFFC9) return 0;
    if (a == 0xFF9B) return 0x04; /* SML2 scrolling gameplay */
    if (a == 0xA28B) return 0;    /* not a bonus/minigame room */
    if (a >= 0xB000 && a < 0xE000) return test_level_map[a - 0xB000];
    if (a >= 0xA600 && a < 0xA800) return test_block_defs[a - 0xA600];
    if (a >= 0xFF80 && a <= 0xFFFE) return test_hram[a - 0xFF80];
    return 0;
}

void hw_tick(int tcycles)
{
    total_cycles += (uint64_t)tcycles;
}

int main(void)
{
    int l, r;
    rom = (uint8_t *)calloc(0x10000, 1);
    if (!rom) return 2;

    rom[0x801C] = 0x19;
    rom[0x8040] = 0x19;
    rom[0x8064] = 0x19;
    cart_hi = rom + 0x8000;

    wide_dims(GAME_SML2, 100, &l, &r);
    if (l != 49 || r != 33) return 3;
    if (!wide_install(GAME_SML2, l, r)) return 4;

    /* Block-map decoding selects the right 8x8 quadrant of a live 16x16 block. */
    test_level_map[1] = 3;
    test_block_defs[3 * 4 + 2] = 0x42;
    uint8_t resolved_tile = 0;
    if (!wide_sml2_tile_at(17, 9, &resolved_tile) || resolved_tile != 0x42) return 30;
    if (wide_sml2_tile_at(-1, 0, &resolved_tile) ||
        wide_sml2_tile_at(4096, 0, &resolved_tile) ||
        wide_sml2_tile_at(0, 768, &resolved_tile)) return 31;

    /* Matching centre tiles unlock margin rendering; the centre itself is excluded. */
    wide_sml2_prepare_frame(l, r, 0x30, 0x00, 0xE3, 0x07, 0x88, vram);
    int world_x = -1, world_y = -1;
    if (!wide_sml2_margin_tile(0, 0, &resolved_tile, &world_x, &world_y) ||
        world_x != 511 || world_y != 0) return 32;
    if (wide_sml2_margin_tile(l, 0, &resolved_tile, &world_x, &world_y)) return 33;

    cpu.pc = 0x401C;
    cpu.h = 1;
    cpu.l = 0;
    cpu.d = 0;
    cpu.e = 0x60;
    cpu.f = 0;
    total_cycles = 0;

    if (!wide_intercept_sml2(0x19)) return 5;
    if (HL() != 0x0181) return 6;
    if (cpu.pc != 0x401D || total_cycles != 8) return 7;

    /* The wider loader also uses the +0x70 and +0xA0 horizontal bounds.
     * Each hook adds the same configured right-side extension (+0x21 here). */
    cpu.pc = 0x4040;
    cpu.h = 1;
    cpu.l = 0;
    cpu.f = 0;
    total_cycles = 0;
    if (!wide_intercept_sml2(0x19)) return 8;
    if (HL() != 0x0191 || cpu.pc != 0x4041 || total_cycles != 8) return 9;

    cpu.pc = 0x4064;
    cpu.h = 1;
    cpu.l = 0;
    cpu.f = 0;
    total_cycles = 0;
    if (!wide_intercept_sml2(0x19)) return 10;
    if (HL() != 0x01C1 || cpu.pc != 0x4065 || total_cycles != 8) return 11;

    /* The entity-loader's activation and cull windows must also follow
     * the widened camera bounds, or actors still pop out before the margin. */
    cpu.pc = 0x3CAA;
    if (wide_read_sml2(0xAF0A, 0) != 0x03 ||
        wide_read_sml2(0xAF0B, 0) != 0x01 ||
        wide_read_sml2(0xAF0C, 0) != 0x01 ||
        wide_read_sml2(0xAF0D, 0) != 0xEF) return 22;
    if (wide_read_sml2(0xAF1A, 0) != 0x03 ||
        wide_read_sml2(0xAF1B, 0) != 0x41 ||
        wide_read_sml2(0xAF1C, 0) != 0x01 ||
        wide_read_sml2(0xAF1D, 0) != 0xAF) return 23;

    /* The widened windows must saturate at the level start, not wrap a
       negative lower bound to $FFxx and spuriously cull nearby actors. */
    test_camera_x = 80;
    if (wide_read_sml2(0xAF0C, 0xFF) != 0x00 ||
        wide_read_sml2(0xAF0D, 0xFF) != 0x00 ||
        wide_read_sml2(0xAF1C, 0xFF) != 0x00 ||
        wide_read_sml2(0xAF1D, 0xFF) != 0x00) return 43;
    test_camera_x = 640;

    /* Enemy spawn scan: the widened edge must advance in <=8 px steps
     * so the ROM cannot consume a spawn-list entry between frames. */
    frame_count = 0;
    cpu.pc = 0x408A;
    if (wide_read_sml2(0xAF12, 0xFF) != 0x02) return 24;
    cpu.pc = 0x4090;
    if (wide_read_sml2(0xAF13, 0xFF) != 0xF8) return 25;
    frame_count++;
    cpu.pc = 0x408A;
    if (wide_read_sml2(0xAF12, 0xFF) != 0x03) return 26;
    cpu.pc = 0x4090;
    if (wide_read_sml2(0xAF13, 0xFF) != 0x00) return 27;
    frame_count++;
    cpu.pc = 0x40A9;
    if (wide_read_sml2(0xAF14, 0xFF) != 0x02) return 28;
    cpu.pc = 0x40AF;
    if (wide_read_sml2(0xAF15, 0xFF) != 0x08) return 29;

    cpu.pc = 0x401D;
    cpu.h = 1;
    cpu.l = 0;
    if (wide_intercept_sml2(0x19)) return 18;
    if (HL() != 0x0100) return 19;

    /* The same CPU opcode must not be intercepted while another bank is mapped. */
    cart_hi = rom + 0xC000;
    cpu.pc = 0x401C;
    cpu.h = 1;
    cpu.l = 0;
    cpu.d = 0;
    cpu.e = 0x60;
    total_cycles = 0;
    if (wide_intercept_sml2(0x19)) return 20;
    if (HL() != 0x0100 || total_cycles != 0) return 21;

    /* Capture one enemy metasprite before the ROM's native-screen clipping. */
    memset(test_hram, 0, sizeof test_hram);
    test_hram[0xFFD0 - 0xFF80] = 0x02; /* actor X = 520 */
    test_hram[0xFFD1 - 0xFF80] = 0x08;
    test_hram[0xFFD3 - 0xFF80] = 0x00; /* actor Y = 72 */
    test_hram[0xFFD4 - 0xFF80] = 0x48;
    test_hram[0xFFDB - 0xFF80] = 0;
    test_scx = 0x30; /* SCX aligned with camera X = $0280 */
    test_hram[0xFFE2 - 0xFF80] = 0;

    /* Widened actors beyond the native strip must not wrap into its centre:
       $A2B1 is overridden only from the real actor-draw bank/call site. */
    cpu.pc = 0x409E;
    if (wide_read_sml2(0xA2B1, 0xAA) != 0x58) return 39; /* X=520 -> rel=-40 */
    test_hram[0xFFD1 - 0xFF80] = 0x80; /* X=640 -> rel=80: keep native draw */
    if (wide_read_sml2(0xA2B1, 0xAA) != 0xAA) return 40;
    test_hram[0xFFD0 - 0xFF80] = 0x03;
    test_hram[0xFFD1 - 0xFF80] = 0x20; /* X=800 -> rel=240 */
    if (wide_read_sml2(0xA2B1, 0xAA) != 0x70) return 41;
    cpu.pc = 0x4080; /* Never alter unrelated reads. */
    if (wide_read_sml2(0xA2B1, 0xAA) != 0xAA) return 42;

    /* Restore X=520 for the margin metasprite-capture test below. */
    test_hram[0xFFD0 - 0xFF80] = 0x02;
    test_hram[0xFFD1 - 0xFF80] = 0x08;
    rom[0xC0B1] = 0x00; rom[0xC0B2] = 0x41; /* bank-3 pointer -> $4100 */
    rom[0xC100] = 0x00;
    rom[0xC101] = 0x00;
    rom[0xC102] = 0x21;
    rom[0xC103] = 0x00;
    rom[0xC104] = 0x80; /* metasprite sentinel */
    cpu.pc = 0x401B;
    if (wide_sml2_read_tap(0xFFE2, 0) != 0) return 34;
    wide_sml2_snapshot_actors();
    int actor_count = 0, world_left = 0, world_top = 0;
    const WideSml2Sprite *actors = wide_sml2_actor_sprites(&actor_count);
    if (!actors || actor_count != 1 || actors[0].x != 520 ||
        actors[0].y != 72 || actors[0].tile != 0x21 || actors[0].attr != 0)
        return 35;
    if (!wide_sml2_world_origin(&world_left, &world_top) ||
        world_left != 511 || world_top != 0) return 36;

    /* Homeworld's raw 64x32 tile map must be read with a 64-tile row stride,
       and its margins must use the map camera at $A680/$A682. */
    test_world_x = 320;
    test_world_y = 32;
    test_scx = 64;
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 64; tx++)
            test_level_map[ty * 0x40 + tx] = (uint8_t)((ty * 3 + tx + 1) & 0x7F);
    }
    memset(vram, 0, sizeof vram);
    for (int sy = 0; sy < 136; sy += 8) {
        for (int sx = 0; sx < 160; sx += 8) {
            unsigned col = (unsigned)(test_world_x + sx) >> 3;
            unsigned row = (unsigned)(test_world_y + sy) >> 3;
            unsigned src = row * 0x40u + col;
            unsigned dst = 0x1800u + (row & 31u) * 32u + (col & 31u);
            vram[dst] = test_level_map[src];
        }
    }
    if (!wide_sml2_homeworld_active(0xE3, 80, 0x88) ||
        wide_sml2_homeworld_active(0xE3, 7, 0x88) ||
        wide_sml2_homeworld_active(0xE3, 80, 0x87)) return 43;
    /* The Homeworld path supports a much wider symmetric margin than regular stages. */
    wide_sml2_prepare_frame(80, 80, 64, 32, 0xE3, 80, 0x88, vram);
    if (!wide_sml2_world_origin(&world_x, &world_y) ||
        world_x != 240 || world_y != 32) return 39;
    if (!wide_sml2_margin_tile(0, 0, &resolved_tile, &world_x, &world_y) ||
        world_x != 240 || world_y != 32 || resolved_tile != 43) return 40;
    if (!wide_sml2_margin_tile(240, 0, &resolved_tile, &world_x, &world_y) ||
        world_x != 480 || world_y != 32 || resolved_tile != 73) return 41;
    if (wide_sml2_margin_tile(80, 0, &resolved_tile, &world_x, &world_y)) return 42;

    /* Switching back to native width must clear the hooks from the prior
       widescreen session, so they do not keep changing SML2 spawn/cull reads. */
    cart_hi = rom + 0x8000;
    if (!wide_install(GAME_SML2, 0, 0)) return 37;
    cpu.pc = 0x408A;
    if (wide_read_sml2(0xAF12, 0xAA) != 0xAA) return 38;

    free(rom);
    puts("SML2 widescreen hook: PASS");
    return 0;
}
