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
static int expect_fill(int addr, uint8_t value, int n)
{
    for (int i = 0; i < n; i++) if (rom[addr + i] != value) return 0;
    return 1;
}

static int sml2_right_extra;
static int sml2_left_extra;
static int sml2_scan_edge[2] = {-1, -1};
static int sml2_scan_pick[2];
static int sml2_scan_frame[2] = {-1, -1};

static int sml2_read16(uint16_t a)
{
    return (int)rd8(a) | ((int)rd8((uint16_t)(a + 1)) << 8);
}

static int sml2_scan_value(int side)
{
    int cam = sml2_read16(0xFFCA);
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
    sml2_scan_frame[side] = frame_count;
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

    int edge = sml2_scan_frame[side] == frame_count
        ? sml2_scan_pick[side] : sml2_scan_value(side);

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


void wide_state_reset(void)
{
    /* These are runtime scanner latches, not emulated RAM. A state load can jump
     * to an unrelated room/frame, so stale bounds from the previous timeline
     * must never survive the load. Keep the installed widescreen dimensions. */
    sml2_scan_edge[0] = sml2_scan_edge[1] = -1;
    sml2_scan_pick[0] = sml2_scan_pick[1] = 0;
    sml2_scan_frame[0] = sml2_scan_frame[1] = -1;
}

int wide_install(int game, int l, int r)
{
    sml2_right_extra = 0;
    sml2_left_extra = 0;
    sml2_scan_edge[0] = sml2_scan_edge[1] = -1;
    sml2_scan_pick[0] = sml2_scan_pick[1] = 0;
    sml2_scan_frame[0] = sml2_scan_frame[1] = -1;
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
        /* Clean SML and the DX-patched image share these code offsets. The clean ROM's
         * unused trampoline area is 0xFF-filled, while the DX IPS patch zeroes it.
         * Accept either known-unused fill pattern, but never overwrite a mixed/used region. */
        static const uint8_t o1[] = {0xFA, 0xAB, 0xC0}, o2[] = {0xC6, 0xD0}, o3[] = {0xF0, 0xC3, 0xFE, 0xE0, 0x38, 0x0A};
        if (!expect(0x249C, o1, 3) || !expect(0x24B6, o2, 2) || !expect(0x257B, o3, 6) ||
            (!expect_fill(0x3FE4, 0x00, 26) && !expect_fill(0x3FE4, 0xFF, 26))) return 0;
        int k = sml_k(r);
        int T = k >= 2 ? 0xC0 + 16 * k + 12 + 1 : 0xE0;
        int U = l > 8 ? 256 - (l - 8) : 0x100;
        if (U - T < sml_gap && l > 8) return 0;
        if (k > 0) {
            const uint8_t t1[] = {0xFA, 0xAB, 0xC0, 0xC6, (uint8_t)k, 0xC9};      /* LD A,(C0AB); ADD A,k; RET */
            memcpy(&rom[0x3FE4], t1, sizeof t1);
            rom[0x249C] = 0xCD; rom[0x249D] = 0xE4; rom[0x249E] = 0x3F;           /* CALL 3FE4 */
            rom[0x24B7] = (uint8_t)(0xD0 + 16 * k);                               /* spawn X + 16k */
        }
        if (l > 8 || k >= 2) {
            const uint8_t t2[] = {0xF0, 0xC3, 0xFE, (uint8_t)T, 0xD8, 0xFE, (uint8_t)(U & 0xFF), 0x3F, 0xC9};
            if (U >= 0x100) { /* no left extension: plain threshold */
                const uint8_t t2b[] = {0xF0, 0xC3, 0xFE, (uint8_t)T, 0xC9};
                memcpy(&rom[0x3FEA], t2b, sizeof t2b);
            } else memcpy(&rom[0x3FEA], t2, sizeof t2);
            rom[0x257B] = 0xCD; rom[0x257C] = 0xEA; rom[0x257D] = 0x3F; rom[0x257E] = 0x00;   /* CALL 3FEA; NOP */
        }
        return 1;
    }
    return 0;
}