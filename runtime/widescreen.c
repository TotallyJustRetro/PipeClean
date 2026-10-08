#include <string.h>
#include <stdlib.h>
#include "widescreen.h"
#include "gb.h"
#include "cart.h"

/* ---- Super Mario Land ----
 * Level columns are streamed 7 columns ahead of the screen and 4 behind, so the background is already
 * correct up to 56 px right / 32 px left of the screen. Enemies are the limit: they live in an 8 bit X
 * range, spawn at X = C0 (just off the right edge) and are removed once X wraps past E0. We
 *   - spawn them k columns (16 px) earlier and 16k px further right,
 *   - remove only entities in [T, U) so a few px on the left stay alive too. */
static const int sml_gap = 8;

static int sml_k(int r) { return r <= 24 ? 0 : (r - 24 + 15) / 16; }

static int sml_lmax(int k)
{
    if (k == 0) return 32;
    if (k == 1) return 32;
    return 19;              /* k == 2: T = ED leaves 256 - ED - gap = 11 px of negative X */
}

void wide_dims(int game, int pct, int *l, int *r)
{
    *l = *r = 0;
    if (pct <= 0) return;
    if (game == GAME_SML2) { *l = (49 * pct + 50) / 100; *r = (33 * pct + 50) / 100; return; }
    if (game == GAME_SML) {
        int rr = (56 * pct + 50) / 100, ll = (32 * pct + 50) / 100;
        int k = sml_k(rr), lm = sml_lmax(k);
        *l = ll < lm ? ll : lm; *r = rr;
    }
}

static int expect(int addr, const uint8_t *b, int n) { return memcmp(&rom[addr], b, (size_t)n) == 0; }

static int sml2_right_extra;
static int sml2_left_extra;
static int sml2_scan_edge[2] = {-1, -1};
static int sml2_scan_pick[2];

typedef struct {
    int x, y;
    uint8_t tile, attr;
} Sml2WideSprite;

#define SML2_WIDE_SPRITES 256
static Sml2WideSprite sml2_wide_sprites[SML2_WIDE_SPRITES];
static int sml2_wide_sprite_count;
static int sml2_emit_active;

static int sml2_rom_byte(unsigned addr)
{
    if (!rom || addr < 0x4000u || addr >= 0x8000u) return 0xFF;
    return rom[addr];
}

static int sml2_pc_is_emitter(uint16_t pc)
{
    /* V1.0 shared emitter taps: PC is already advanced past LD A,[$FFC5]. */
    return pc == 0x52BA || pc == 0x5E5D || pc == 0x5D8B || pc == 0x5E3F;
}

static void sml2_capture_emitter(uint8_t sx)
{
    if (!sml2_pc_is_emitter(cpu.pc) || cart_hi != rom + 0x4000) return;

    int sy = rd8(0xFFC4);
    int idx = rd8(0xFFC6);
    int pal = rd8(0xFFC7) != 0;

    int xs = sx >= 0xD0 ? (int)sx - 256 : (int)sx;
    int ys = sy >= 0xD0 ? (int)sy - 256 : (int)sy;
    int ox = ppu_xoff + xs - 8;
    int oy = ys - 16;

    unsigned entry = 0x4000u + (unsigned)idx * 2u;
    if (entry + 1 >= 0x8000u) return;
    unsigned de = (unsigned)sml2_rom_byte(entry) |
                  ((unsigned)sml2_rom_byte(entry + 1) << 8);
    if (de < 0x4000u || de + 3 >= 0x8000u) return;

    for (int n = 0; n < 64 && de + 3 < 0x8000u; n++, de += 4) {
        uint8_t yraw = (uint8_t)sml2_rom_byte(de);
        if (yraw == 0x80) break;
        if (sml2_wide_sprite_count >= SML2_WIDE_SPRITES) break;

        Sml2WideSprite *sp = &sml2_wide_sprites[sml2_wide_sprite_count++];
        sp->y = oy + (int8_t)yraw;
        sp->x = ox + (int8_t)sml2_rom_byte(de + 1);
        sp->tile = (uint8_t)sml2_rom_byte(de + 2);
        sp->attr = (uint8_t)sml2_rom_byte(de + 3);
        if (pal) sp->attr |= 0x10;
    }
    sml2_emit_active = 1;
}


static int sml2_camera_x(void)
{
    /* SML2 stores camera X in HRAM $FFCA/$FFCB. Read the backing HRAM
     * directly so the standalone widescreen regressions stay independent of
     * the full CPU-memory API. */
    return (int)hram[0x4A] | ((int)hram[0x4B] << 8);
}

static int sml2_scan_value(int side)
{
    int cam = sml2_camera_x();
    int vanilla = side == 0 ? cam + 112 : cam - 112;
    int target = side == 0 ? vanilla + sml2_right_extra : vanilla - sml2_left_extra;
    if (target < 0) target = 0;
    if (target > 0xFFFF) target = 0xFFFF;

    int prev = sml2_scan_edge[side];
    if (prev < 0 || abs(prev - vanilla) > 128) prev = vanilla;

    int edge = target;
    if (side == 0 && edge > prev + 8) edge = prev + 8;
    if (side == 1 && edge < prev - 8) edge = prev - 8;
    if (edge < 0) edge = 0;
    if (edge > 0xFFFF) edge = 0xFFFF;

    sml2_scan_edge[side] = edge;
    sml2_scan_pick[side] = edge;
    return edge;
}

/* SML2's enemy scanner consumes spawn-list entries as it advances. The ROM
 * aligns its scan edge to 8 px, so widening the edge in one jump can step over
 * an entry before the next frame. Feed each scan a latched edge that moves
 * toward the widened target by at most one 8 px quantum per scan. */
uint8_t wide_read_sml2(uint16_t address, uint8_t value)
{
    if (!sml2_right_extra && !sml2_left_extra) return value;
    if (cart_hi != rom + 0x8000) return value;

    /* The entity-loader rebuilds both its activation and cull windows from
     * the camera every frame. These are RAM-backed bounds, so widening the
     * final ADD HL,DE alone leaves the next window narrower and causes actors
     * to disappear before they reach the visible margin. Override the four
     * bytes at the ROM0 memcpy call site, using the exact same camera-relative
     * bounds the hardware logic uses. */
    if (cpu.pc == 0x3CAA || cpu.pc == 0x3CAB) {
        int cam = sml2_read16(0xFFCA);
        unsigned upper = (unsigned)(cam + 0x60 + sml2_right_extra);
        unsigned lower = (unsigned)(cam - 0x60 - sml2_left_extra);
        int cull = address >= 0xAF1A && address <= 0xAF1D;
        if (cull) {
            upper = (unsigned)(cam + 0xA0 + sml2_right_extra);
            lower = (unsigned)(cam - 0xA0 - sml2_left_extra);
        }
        if (address == 0xAF0A || address == 0xAF1A) return (uint8_t)(upper >> 8);
        if (address == 0xAF0B || address == 0xAF1B) return (uint8_t)upper;
        if (address == 0xAF0C || address == 0xAF1C) return (uint8_t)(lower >> 8);
        if (address == 0xAF0D || address == 0xAF1D) return (uint8_t)lower;
    }

    int side = -1;
    uint16_t hi_pc = 0, lo_pc = 0;
    if (address == 0xAF12 || address == 0xAF13) {
        side = 0; hi_pc = 0x408A; lo_pc = 0x4090;
    } else if (address == 0xAF14 || address == 0xAF15) {
        side = 1; hi_pc = 0x40A9; lo_pc = 0x40AF;
    }
    if (side < 0) return value;

    if (cpu.pc != hi_pc && cpu.pc != lo_pc &&
        cpu.pc != (uint16_t)(hi_pc + 3) &&
        cpu.pc != (uint16_t)(lo_pc + 3))
        return value;

    int edge;
    if (address == 0xAF12 || address == 0xAF14) {
        edge = sml2_scan_value(side);
    } else {
        edge = sml2_scan_pick[side];
    }

    return address == 0xAF12 || address == 0xAF14
        ? (uint8_t)(edge >> 8)
        : (uint8_t)edge;
}


/* The SML2 entity-loader builds several horizontal look-ahead limits from
 * ADD HL,DE in the same bank-2 routine.  Different ROM revisions can move
 * the exact surrounding instructions, so the hook is deliberately guarded
 * by the opcode at the expected address before touching a particular PC. */
static unsigned sml2_hook_base(uint16_t pc)
{
    switch (pc) {
    case 0x401C: return 0x0060u;
    case 0x4040: return 0x0070u;
    case 0x4064: return 0x00A0u;
    default: return 0;
    }
}

void wide_tap_sml2(uint16_t address, uint8_t value)
{
    if (address == 0xFFC5 && sml2_right_extra) sml2_capture_emitter(value);
}

void wide_sml2_begin_frame(void)
{
    sml2_wide_sprite_count = 0;
    sml2_emit_active = 0;
}

void wide_sml2_compose_margins(void)
{
    if (!sml2_right_extra || !sml2_wide_sprite_count) return;

    const int left = ppu_xoff;
    const int native_right = left + GB_W;

    for (int n = 0; n < sml2_wide_sprite_count; n++) {
        const Sml2WideSprite *sp = &sml2_wide_sprites[n];
        if (sp->x + 7 >= left && sp->x < native_right) continue;
        if (sp->x + 7 < 0 || sp->x >= ppu_w) continue;

        for (int py = 0; py < 8; py++) {
            int y = sp->y + py;
            if ((unsigned)y >= GB_H) continue;
            int row = (sp->attr & 0x40) ? 7 - py : py;
            int addr = (sp->tile * 16) + row * 2;
            for (int px = 0; px < 8; px++) {
                int x = sp->x + px;
                if ((unsigned)x >= (unsigned)ppu_w) continue;
                int bit = (sp->attr & 0x20) ? px : 7 - px;
                int ci = (((vram[addr + 1] >> bit) & 1) << 1) |
                         ((vram[addr] >> bit) & 1);
                if (!ci) continue;
                if ((sp->attr & 0x80) && ppu_shade[y][x] != 0) continue;

                uint8_t pal = (sp->attr & 0x10) ? 2 : 1;
                /* Use a raw shade index here; render_build maps it through the
                 * configured PipeClean palette exactly like normal sprites. */
                (void)pal;
                ppu_shade[y][x] = ci;
                ppu_layer[y][x] = pal;
                ppu_sprtile[y][x] = (uint16_t)(addr >> 4);
                ppu_spruv[y][x] = (uint8_t)((row << 3) | (7 - bit) |
                    ((sp->attr & 0x20) ? 0x40 : 0) |
                    ((sp->attr & 0x40) ? 0x80 : 0));
            }
        }
    }
}

int wide_intercept_sml2(uint8_t op)
{
    if (!sml2_right_extra || op != 0x19) return 0;
    /* These are the three horizontal ADD HL,DE look-ahead limits in the
     * bank-2 entity setup routine.  Do not touch similarly shaped code in
     * another bank or a ROM with a different instruction at the hook PC. */
    unsigned base = sml2_hook_base(cpu.pc);
    if (!base || cart_hi != rom + 0x8000) return 0;
    size_t off = 0x8000u + (size_t)(cpu.pc - 0x4000u);
    if (rom[off] != 0x19) return 0;

    unsigned hl = (unsigned)((cpu.h << 8) | cpu.l);
    unsigned de = base + (unsigned)sml2_right_extra;
    unsigned sum = hl + de;
    cpu.f = (uint8_t)((cpu.f & 0x80)
        | ((((hl & 0x0FFFu) + (de & 0x0FFFu)) > 0x0FFFu) ? 0x20 : 0)
        | ((sum > 0xFFFFu) ? 0x10 : 0));
    cpu.h = (uint8_t)(sum >> 8);
    cpu.l = (uint8_t)sum;
    cpu.pc = (uint16_t)(cpu.pc + 1);
    hw_tick(8);
    return 1;
}


int wide_install(int game, int l, int r)
{
    sml2_right_extra = 0;
    sml2_left_extra = 0;
    sml2_scan_edge[0] = sml2_scan_edge[1] = -1;
    sml2_scan_pick[0] = sml2_scan_pick[1] = 0;
    if (l + r == 0) return 1;
    if (game == GAME_SML2) {
        /* SML2's entity activation code lives in bank 2.  Its horizontal
         * look-ahead bounds are formed by a small sequence of ADD HL,DE
         * instructions.  We intercept those instructions instead of rewriting
         * a variable-length instruction stream in the ROM.  This is especially
         * appropriate here because SML2 already runs through the interpreter. */
        if (r > 0x9F || rom[0x801C] != 0x19) return 0;
        sml2_right_extra = r;
        sml2_left_extra = l;
        return 1;
    }

    if (game == GAME_SML) {
        static const uint8_t o1[] = {0xFA, 0xAB, 0xC0}, o2[] = {0xC6, 0xD0}, o3[] = {0xF0, 0xC3, 0xFE, 0xE0, 0x38, 0x0A};
        static const uint8_t zero[26] = {0};
        if (!expect(0x24A5, o1, 3) || !expect(0x24BF, o2, 2) || !expect(0x2584, o3, 6) || !expect(0x3FE4, zero, 26)) return 0;
        int k = sml_k(r);
        int T = k >= 2 ? 0xC0 + 16 * k + 12 + 1 : 0xE0;
        int U = l > 8 ? 256 - (l - 8) : 0x100;
        if (U - T < sml_gap && l > 8) return 0;
        if (k > 0) {
            const uint8_t t1[] = {0xFA, 0xAB, 0xC0, 0xC6, (uint8_t)k, 0xC9};      /* LD A,(C0AB); ADD A,k; RET */
            memcpy(&rom[0x3FE4], t1, sizeof t1);
            rom[0x24A5] = 0xCD; rom[0x24A6] = 0xE4; rom[0x24A7] = 0x3F;           /* CALL 3FE4 */
            rom[0x24C0] = (uint8_t)(0xD0 + 16 * k);                               /* spawn X + 16k */
        }
        if (l > 8 || k >= 2) {
            const uint8_t t2[] = {0xF0, 0xC3, 0xFE, (uint8_t)T, 0xD8, 0xFE, (uint8_t)(U & 0xFF), 0x3F, 0xC9};
            if (U >= 0x100) { /* no left extension: plain threshold */
                const uint8_t t2b[] = {0xF0, 0xC3, 0xFE, (uint8_t)T, 0xC9};
                memcpy(&rom[0x3FEA], t2b, sizeof t2b);
            } else memcpy(&rom[0x3FEA], t2, sizeof t2);
            rom[0x2584] = 0xCD; rom[0x2585] = 0xEA; rom[0x2586] = 0x3F; rom[0x2587] = 0x00;   /* CALL 3FEA; NOP */
        }
        return 1;
    }
    return 0;
}