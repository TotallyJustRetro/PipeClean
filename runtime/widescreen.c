#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "widescreen.h"
#include "gb.h"
#include "cart.h"

#if defined(__GNUC__) || defined(__clang__)
/* Standalone widescreen regression tests compile this file without cart.c.
 * Keep the WL2 map reader optional in those test binaries. */
extern uint8_t *cart_ram __attribute__((weak));
extern const CartInfo *cart_info(void) __attribute__((weak));
extern uint8_t gb_wram_peek(size_t offset) __attribute__((weak));
#endif

/* The wider PPU can reveal areas outside the game's original actor window.
 * Trace executed comparison sites while widescreen is enabled, recording
 * value ranges and conditional branches. This is diagnostics only: it never
 * modifies the ROM or CPU state. */
#define WL2_TRACE_SLOTS 4096u
#define WL2_OAM_EVENT_SLOTS 4096u

typedef struct {
    uint32_t frame;
    uint16_t px, py;
    int16_t old_sx, old_sy, sx, sy;
    uint8_t camx, camy, scx, scy;
    uint8_t sprite, from_region, to_region;
    uint8_t old_y, old_x, old_tile, old_attr;
    uint8_t y, x, tile, attr;
} Wl2OamEvent;

typedef struct {
    uint32_t rom_offset, hits, first_frame, last_frame;
    uint32_t cmp_lt, cmp_eq, cmp_gt, branch_checks, branch_taken;
    uint16_t pc;
    uint16_t bc_min, bc_max, de_min, de_max, hl_min, hl_max;
    uint16_t px_min, px_max, py_min, py_max;
    uint8_t opcode, code[8], operand, operand_min, operand_max;
    uint8_t a, f, b, c, d, e, h, l, scx, scy, camx, camy;
    uint8_t a_min, a_max, scx_min, scx_max, scy_min, scy_max;
    uint8_t camx_min, camx_max, camy_min, camy_max;
    int used;
} Wl2TraceEntry;

int wide_wl2_trace_active;
/* The world-map/compositor profile must remain active independently of diagnostics. */
static int wl2_profile_active;
static int wl2_profile_game = -1;
static Wl2TraceEntry wl2_trace_entries[WL2_TRACE_SLOTS];
static size_t wl2_trace_count;
static int wl2_trace_atexit_registered;
static int wl2_trace_game, wl2_trace_left, wl2_trace_right;
static uint32_t wl2_trace_crc;
static char wl2_trace_path[1200] = "pipeclean-wl2-wide-trace.txt";
static size_t wl2_trace_rom_size;
static uint8_t wl2_trace_cgb_flag;
static char wl2_trace_title[17];
static int wl2_trace_pending_valid;
static size_t wl2_trace_pending_slot;
static uint16_t wl2_trace_pending_next_pc;
static Wl2OamEvent wl2_oam_events[WL2_OAM_EVENT_SLOTS];
static size_t wl2_oam_event_count;
static uint32_t wl2_oam_events_dropped;
static uint8_t wl2_oam_previous[0xA0];
static int wl2_oam_previous_valid;
static char wl2_oam_path[1200] = "pipeclean-wl2-oam-events.txt";

/* Resolve a Wario Land II 16x16 world block using the game's nibble-swizzled
 * room map. The map can occupy several cartridge-RAM banks, so read the same
 * linear Cart RAM offset used by the game's RAM domain rather than whichever
 * SRAM bank happens to be selected by the emulated CPU. */
static uint8_t wl2_swap_nibbles(uint8_t v)
{
    return (uint8_t)((v << 4) | (v >> 4));
}

int wide_wl2_block_at(int world_x, int world_y, uint8_t *block)
{
    if (!wl2_profile_active || !block || !rom ||
        (wl2_profile_game != GAME_WARIO_LAND2_GB &&
         wl2_profile_game != GAME_WARIO_LAND2_GBC) ||
        world_x < 0 || world_y < 0 || world_x > 0xFFFF || world_y > 0xFFFF)
        return 0;
#if defined(__GNUC__) || defined(__clang__)
    if (&cart_ram == NULL || cart_info == NULL) return 0;
#endif
    uint8_t *ram = cart_ram;
    const CartInfo *ci = cart_info();
    if (!ram || !ci) return 0;

    uint8_t hx = (uint8_t)((unsigned)world_x >> 8);
    uint8_t hy = (uint8_t)((unsigned)world_y >> 8);
    uint8_t lx = (uint8_t)world_x;
    uint8_t ly = (uint8_t)world_y;
    unsigned row = (unsigned)wl2_swap_nibbles((uint8_t)(hy & 0x0Fu)) +
                   (unsigned)(wl2_swap_nibbles(ly) & 0x0Fu) + 0xA0u;
    unsigned col = (unsigned)wl2_swap_nibbles((uint8_t)(hx & 0x0Fu)) +
                   (unsigned)(wl2_swap_nibbles(lx) & 0x0Fu);
    uint16_t address = (uint16_t)((((row & 0xFFu) << 8) | (col & 0xFFu)));

    if (address >= 0xA000u) {
        size_t offset = (size_t)address - 0x8000u;
        if (offset >= ci->ram_bytes) return 0;
        /* The top bit is a map flag, not a metatile index. WL2 uses
           a 128-entry, four-tile table selected by the active room. */
        *block = (uint8_t)(ram[offset] & 0x7Fu);
        return 1;
    }

    /* The documented mapping falls back to the normal bus for exceptional
       rooms whose swizzled address wraps below A000. */
    *block = (uint8_t)(rd8(address) & 0x7Fu);
    return 1;
}

/* Data Crystal's WL2 ROM map identifies 0x18000 as the metatile
 * table. Each 16x16 block has four 8x8 tile IDs; block bit 7 is a world-map
 * flag, so the renderer must use only the low seven bits as the table index.
 * Guard exact supported dumps so altered revisions use the learned fallback. */
int wide_wl2_block_tile(int table_set, int block, int part, uint8_t *tile)
{
    if (!tile || table_set < 0 || table_set >= 32 ||
        block < 0 || block >= 128 || part < 0 || part >= 4 ||
        !wl2_profile_active || !rom)
        return 0;
#if defined(__GNUC__) || defined(__clang__)
    if (cart_info == NULL) return 0;
#endif
    const CartInfo *ci = cart_info();
    if (!ci) return 0;
    if (wl2_profile_game == GAME_WARIO_LAND2_GB) {
        if (ci->crc != 0x9C54358Du || ci->size != 0x100000u ||
            (rom[0x143] & 0x80u) != 0)
            return 0;
    } else if (wl2_profile_game == GAME_WARIO_LAND2_GBC) {
        if (ci->crc != 0x047BDF80u || ci->size != 0x200000u ||
            (rom[0x143] & 0x80u) == 0)
            return 0;
    } else {
        return 0;
    }
    /* The metatile bank contains 32 tables of 0x200 bytes. Each table
       maps the 128 block IDs to four 8x8 tile IDs; PipeClean selects the
       active table by comparing it against native-viewport samples. */
    size_t offset = 0x18000u + (size_t)table_set * 0x200u +
                    (size_t)block * 4u + (size_t)part;
    if (offset >= 0x1C000u || offset >= ci->size) return 0;
    *tile = rom[offset];
    return 1;
}

/* Wario Land II GBC stores a parallel 16 KiB palette-ID bank at
 * 0xF0000. Like the graphic metatile tables at 0x18000, it is arranged as
 * 32 sets of 0x200 bytes, each containing 128 blocks x four 8x8 components.
 * These are the authoritative palette IDs for the extended world; unlike the
 * 256px CGB attribute map in VRAM, they do not get recycled as the camera moves. */
int wide_wl2_block_attr(int table_set, int block, int part, uint8_t *attr)
{
    if (!attr || table_set < 0 || table_set >= 32 ||
        block < 0 || block >= 128 || part < 0 || part >= 4 ||
        !wl2_profile_active || !rom ||
        wl2_profile_game != GAME_WARIO_LAND2_GBC)
        return 0;
#if defined(__GNUC__) || defined(__clang__)
    if (cart_info == NULL) return 0;
#endif
    const CartInfo *ci = cart_info();
    if (!ci || ci->crc != 0x047BDF80u || ci->size != 0x200000u ||
        (rom[0x143] & 0x80u) == 0)
        return 0;
    size_t offset = 0xF0000u + (size_t)table_set * 0x200u +
                    (size_t)block * 4u + (size_t)part;
    if (offset >= 0xF4000u || offset >= ci->size || rom[offset] > 7u)
        return 0;
    *attr = rom[offset];
    return 1;
}

static int wl2_trace_candidate_opcode(uint8_t op)
{
    return op == 0x19 || op == 0xC6 || op == 0xD6 || op == 0xFE ||
           (op >= 0x80 && op <= 0x87) ||
           (op >= 0x90 && op <= 0x97) ||
           (op >= 0xB8 && op <= 0xBF);
}

static int wl2_trace_is_compare(uint8_t op)
{
    return op == 0xFE || (op >= 0xB8 && op <= 0xBF);
}

static int wl2_trace_conditional_branch(uint8_t op)
{
    return op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38 ||
           op == 0xC0 || op == 0xC2 || op == 0xC4 || op == 0xC8 ||
           op == 0xCA || op == 0xCC || op == 0xD0 || op == 0xD2 ||
           op == 0xD4 || op == 0xD8 || op == 0xDA || op == 0xDC;
}

static int wl2_trace_branch_taken(uint8_t op, uint8_t flags)
{
    switch (op) {
    case 0x20: case 0xC0: case 0xC2: case 0xC4: return (flags & 0x80u) == 0; /* NZ */
    case 0x28: case 0xC8: case 0xCA: case 0xCC: return (flags & 0x80u) != 0; /* Z */
    case 0x30: case 0xD0: case 0xD2: case 0xD4: return (flags & 0x10u) == 0; /* NC */
    case 0x38: case 0xD8: case 0xDA: case 0xDC: return (flags & 0x10u) != 0; /* C */
    default: return 0;
    }
}

static int wl2_trace_rom_offset(uint16_t pc, size_t *offset)
{
    if (!rom || !offset || pc >= 0x8000u) return 0;
    const uint8_t *mapped = pc < 0x4000u ? cart_lo : cart_hi;
    if (!mapped || (uintptr_t)mapped < (uintptr_t)rom) return 0;
    size_t base = (size_t)((uintptr_t)mapped - (uintptr_t)rom);
    size_t physical = base + (size_t)(pc & 0x3FFFu);
    if (physical >= wl2_trace_rom_size) return 0;
    *offset = physical;
    return 1;
}

static uint8_t wl2_trace_register_operand(uint8_t op)
{
    switch (op & 7u) {
    case 0: return cpu.b;
    case 1: return cpu.c;
    case 2: return cpu.d;
    case 3: return cpu.e;
    case 4: return cpu.h;
    case 5: return cpu.l;
    case 6: return rd8((uint16_t)(((uint16_t)cpu.h << 8) | cpu.l));
    default: return cpu.a;
    }
}

static void wl2_trace_u8_range(uint8_t v, uint8_t *lo, uint8_t *hi)
{
    if (v < *lo) *lo = v;
    if (v > *hi) *hi = v;
}

static void wl2_trace_u16_range(uint16_t v, uint16_t *lo, uint16_t *hi)
{
    if (v < *lo) *lo = v;
    if (v > *hi) *hi = v;
}

void wide_wl2_trace_instruction(uint16_t pc)
{
    if (!wide_wl2_trace_active || !rom) {
        wl2_trace_pending_valid = 0;
        return;
    }

    size_t off;
    if (!wl2_trace_rom_offset(pc, &off)) {
        wl2_trace_pending_valid = 0;
        return;
    }
    uint8_t op = rom[off];

    /* When a compare is immediately followed by JR/JP/RET cc, the flags at
       this hook still belong to that compare. Record the actual decision. */
    if (wl2_trace_pending_valid) {
        if (pc == wl2_trace_pending_next_pc && wl2_trace_conditional_branch(op) &&
            wl2_trace_pending_slot < WL2_TRACE_SLOTS) {
            Wl2TraceEntry *prior = &wl2_trace_entries[wl2_trace_pending_slot];
            if (prior->used) {
                prior->branch_checks++;
                if (wl2_trace_branch_taken(op, cpu.f)) prior->branch_taken++;
            }
        }
        wl2_trace_pending_valid = 0;
    }

    if (!wl2_trace_candidate_opcode(op)) return;

    uint32_t hash = (uint32_t)(off ^ (off >> 11) ^ ((uint32_t)op * 2654435761u));
    size_t slot = (size_t)hash & (WL2_TRACE_SLOTS - 1u);
    Wl2TraceEntry *entry = NULL;
    for (size_t probe = 0; probe < WL2_TRACE_SLOTS; probe++) {
        size_t candidate_slot = (slot + probe) & (WL2_TRACE_SLOTS - 1u);
        Wl2TraceEntry *candidate = &wl2_trace_entries[candidate_slot];
        if (!candidate->used) {
            if (wl2_trace_count >= WL2_TRACE_SLOTS) return;
            memset(candidate, 0, sizeof *candidate);
            candidate->used = 1;
            candidate->rom_offset = (uint32_t)off;
            candidate->pc = pc;
            candidate->opcode = op;
            candidate->first_frame = frame_count < 0 ? 0u : (uint32_t)frame_count;
            for (unsigned i = 0; i < sizeof candidate->code; i++)
                candidate->code[i] = off + i < wl2_trace_rom_size ? rom[off + i] : 0xFFu;
            wl2_trace_count++;
            entry = candidate;
            slot = candidate_slot;
            break;
        }
        if (candidate->rom_offset == off && candidate->opcode == op) {
            entry = candidate;
            slot = candidate_slot;
            break;
        }
    }
    if (!entry) return;

    uint8_t operand;
    if (op == 0xFE || op == 0xC6 || op == 0xD6)
        operand = off + 1u < wl2_trace_rom_size ? rom[off + 1u] : 0xFFu;
    else if ((op >= 0x80 && op <= 0x87) ||
             (op >= 0x90 && op <= 0x97) ||
             (op >= 0xB8 && op <= 0xBF))
        operand = wl2_trace_register_operand(op);
    else
        operand = cpu.e;

    uint8_t scx = rd8(0xFF43u), scy = rd8(0xFF42u);
    const int color_release = wl2_trace_game == GAME_WARIO_LAND2_GBC;
    /* The TAS navigation notes use different RAM layouts for the two builds:
       GBC player X/Y are D53C/D53A and camera X/Y are C660/C65F; GB player
       X/Y are C53C/C53A and camera X/Y are D652/D651. */
    uint16_t px_addr = color_release ? 0xD53Cu : 0xC53Cu;
    uint16_t py_addr = color_release ? 0xD53Au : 0xC53Au;
    uint16_t camx_addr = color_release ? 0xC660u : 0xD652u;
    uint16_t camy_addr = color_release ? 0xC65Fu : 0xD651u;
    uint8_t camx = rd8(camx_addr), camy = rd8(camy_addr);
    uint16_t bc = (uint16_t)(((uint16_t)cpu.b << 8) | cpu.c);
    uint16_t de = (uint16_t)(((uint16_t)cpu.d << 8) | cpu.e);
    uint16_t hl = (uint16_t)(((uint16_t)cpu.h << 8) | cpu.l);
    uint16_t px = (uint16_t)(((uint16_t)rd8(px_addr) << 8) | rd8((uint16_t)(px_addr + 1u)));
    uint16_t py = (uint16_t)(((uint16_t)rd8(py_addr) << 8) | rd8((uint16_t)(py_addr + 1u)));

    uint32_t frame = frame_count < 0 ? 0u : (uint32_t)frame_count;
    if (entry->hits == 0) {
        entry->a_min = entry->a_max = cpu.a;
        entry->operand_min = entry->operand_max = operand;
        entry->bc_min = entry->bc_max = bc;
        entry->de_min = entry->de_max = de;
        entry->hl_min = entry->hl_max = hl;
        entry->px_min = entry->px_max = px;
        entry->py_min = entry->py_max = py;
        entry->scx_min = entry->scx_max = scx;
        entry->scy_min = entry->scy_max = scy;
        entry->camx_min = entry->camx_max = camx;
        entry->camy_min = entry->camy_max = camy;
    } else {
        wl2_trace_u8_range(cpu.a, &entry->a_min, &entry->a_max);
        wl2_trace_u8_range(operand, &entry->operand_min, &entry->operand_max);
        wl2_trace_u16_range(bc, &entry->bc_min, &entry->bc_max);
        wl2_trace_u16_range(de, &entry->de_min, &entry->de_max);
        wl2_trace_u16_range(hl, &entry->hl_min, &entry->hl_max);
        wl2_trace_u16_range(px, &entry->px_min, &entry->px_max);
        wl2_trace_u16_range(py, &entry->py_min, &entry->py_max);
        wl2_trace_u8_range(scx, &entry->scx_min, &entry->scx_max);
        wl2_trace_u8_range(scy, &entry->scy_min, &entry->scy_max);
        wl2_trace_u8_range(camx, &entry->camx_min, &entry->camx_max);
        wl2_trace_u8_range(camy, &entry->camy_min, &entry->camy_max);
    }

    entry->hits++;
    entry->last_frame = frame;
    entry->operand = operand;
    entry->a = cpu.a; entry->f = cpu.f;
    entry->b = cpu.b; entry->c = cpu.c;
    entry->d = cpu.d; entry->e = cpu.e;
    entry->h = cpu.h; entry->l = cpu.l;
    entry->scx = scx; entry->scy = scy;
    entry->camx = camx; entry->camy = camy;

    if (wl2_trace_is_compare(op)) {
        if (cpu.a < operand) entry->cmp_lt++;
        else if (cpu.a > operand) entry->cmp_gt++;
        else entry->cmp_eq++;
        wl2_trace_pending_valid = 1;
        wl2_trace_pending_slot = slot;
        wl2_trace_pending_next_pc = (uint16_t)(pc + (op == 0xFE ? 2u : 1u));
    }
}

static int wl2_trace_hit_order(const void *a, const void *b)
{
    const Wl2TraceEntry *const *ea = (const Wl2TraceEntry *const *)a;
    const Wl2TraceEntry *const *eb = (const Wl2TraceEntry *const *)b;
    if ((*ea)->hits < (*eb)->hits) return 1;
    if ((*ea)->hits > (*eb)->hits) return -1;
    if ((*ea)->rom_offset < (*eb)->rom_offset) return -1;
    if ((*ea)->rom_offset > (*eb)->rom_offset) return 1;
    return 0;
}

void wide_wl2_trace_set_path(const char *path)
{
    if (!path || !path[0]) return;

    /* Keep GB and GBC captures separately so testing the second release never
       overwrites the first. The launcher passes a path beside its executable;
       preserve only that directory and choose a version-specific filename. */
    const char *forward = strrchr(path, '/');
    const char *backward = strrchr(path, '\\');
    const char *separator = forward;
    if (!separator || (backward && backward > separator)) separator = backward;
    size_t directory_len = separator ? (size_t)(separator - path + 1) : 0;
    if (directory_len >= sizeof wl2_trace_path) directory_len = sizeof wl2_trace_path - 1;
    char directory[1200];
    memcpy(directory, path, directory_len);
    directory[directory_len] = '\0';

    const char *filename = wl2_trace_game == GAME_WARIO_LAND2_GBC
        ? "pipeclean-wl2-gbc-wide-trace.txt"
        : "pipeclean-wl2-gb-wide-trace.txt";
    const char *oam_filename = wl2_trace_game == GAME_WARIO_LAND2_GBC
        ? "pipeclean-wl2-gbc-oam-events.txt"
        : "pipeclean-wl2-gb-oam-events.txt";
    snprintf(wl2_trace_path, sizeof wl2_trace_path, "%s%s", directory, filename);
    snprintf(wl2_oam_path, sizeof wl2_oam_path, "%s%s", directory, oam_filename);
}


static int wl2_oam_region(uint8_t raw_y, uint8_t raw_x, int *screen_x, int *screen_y)
{
    int x = (int)raw_x;
    int neg = wl2_trace_left > 8 ? 256 - (wl2_trace_left - 8) : 256;
    if (x >= neg) x -= 256;
    x = x - 8 + wl2_trace_left;
    int y = (int)raw_y - 16;
    if (screen_x) *screen_x = x;
    if (screen_y) *screen_y = y;

    /* These match the PPU's OAM coordinate transform. An object is logged
       when it enters/leaves the rendered canvas or crosses a margin boundary. */
    int total_w = 160 + wl2_trace_left + wl2_trace_right;
    if (raw_y == 0 || raw_y >= 160 || x + 8 <= 0 || x >= total_w) return 0;
    if (x < wl2_trace_left) return 1;                    /* left extension */
    if (x + 8 > wl2_trace_left + 160) return 3;          /* right extension */
    return 2;                                            /* native viewport */
}

static const char *wl2_oam_region_name(uint8_t region)
{
    switch (region) {
    case 1: return "LEFT";
    case 2: return "NATIVE";
    case 3: return "RIGHT";
    default: return "OFF";
    }
}

static uint8_t wl2_trace_read_ram(uint16_t address)
{
    /* The published WL2 RAM map lists addresses as offsets into the physical
       WRAM domain. Reading Dxxx through rd8() uses the currently selected CGB
       SVBK bank; the game can change SVBK before this frame snapshot. */
#if defined(__GNUC__) || defined(__clang__)
    if (gb_wram_peek && address >= 0xC000u && address < 0xE000u)
        return gb_wram_peek((size_t)(address - 0xC000u));
#endif
    return rd8(address);
}

void wide_wl2_trace_frame(const uint8_t *snapshot)
{
    if (!wide_wl2_trace_active || !snapshot) return;

    const int color_release = wl2_trace_game == GAME_WARIO_LAND2_GBC;
    uint16_t px_addr = color_release ? 0xD53Cu : 0xC53Cu;
    uint16_t py_addr = color_release ? 0xD53Au : 0xC53Au;
    uint16_t camx_addr = color_release ? 0xC660u : 0xD652u;
    uint16_t camy_addr = color_release ? 0xC65Fu : 0xD651u;
    uint16_t px = (uint16_t)(((uint16_t)wl2_trace_read_ram(px_addr) << 8) |
                              wl2_trace_read_ram((uint16_t)(px_addr + 1u)));
    uint16_t py = (uint16_t)(((uint16_t)wl2_trace_read_ram(py_addr) << 8) |
                              wl2_trace_read_ram((uint16_t)(py_addr + 1u)));
    uint8_t camx = wl2_trace_read_ram(camx_addr), camy = wl2_trace_read_ram(camy_addr);
    uint8_t scx = rd8(0xFF43u), scy = rd8(0xFF42u);
    uint32_t frame = frame_count < 0 ? 0u : (uint32_t)frame_count;

    for (unsigned i = 0; i < 40; i++) {
        size_t base = i * 4u;
        uint8_t y = snapshot[base], x = snapshot[base + 1u];
        uint8_t tile = snapshot[base + 2u], attr = snapshot[base + 3u];
        uint8_t old_y = wl2_oam_previous[base], old_x = wl2_oam_previous[base + 1u];
        uint8_t old_tile = wl2_oam_previous[base + 2u], old_attr = wl2_oam_previous[base + 3u];
        int sx = 0, sy = 0, old_sx = 0, old_sy = 0;
        uint8_t to_region = (uint8_t)wl2_oam_region(y, x, &sx, &sy);
        uint8_t from_region = wl2_oam_previous_valid
            ? (uint8_t)wl2_oam_region(old_y, old_x, &old_sx, &old_sy) : 0;

        /* Keep the log focused on visibility/activation transitions. Ordinary
           tile animation doesn't count; large position jumps may mean a slot
           was recycled for a newly activated object. */
        int record = 0;
        if (!wl2_oam_previous_valid) {
            record = to_region != 0;
        } else if (from_region != to_region) {
            record = 1;
        } else if (to_region != 0 &&
                   (abs(sx - old_sx) > 8 || abs(sy - old_sy) > 8)) {
            record = 1;
        }
        if (!record) continue;

        if (wl2_oam_event_count >= WL2_OAM_EVENT_SLOTS) {
            wl2_oam_events_dropped++;
            continue;
        }
        Wl2OamEvent *e = &wl2_oam_events[wl2_oam_event_count++];
        e->frame = frame;
        e->px = px; e->py = py;
        e->old_sx = (int16_t)old_sx; e->old_sy = (int16_t)old_sy;
        e->sx = (int16_t)sx; e->sy = (int16_t)sy;
        e->camx = camx; e->camy = camy; e->scx = scx; e->scy = scy;
        e->sprite = (uint8_t)i;
        e->from_region = from_region; e->to_region = to_region;
        e->old_y = old_y; e->old_x = old_x; e->old_tile = old_tile; e->old_attr = old_attr;
        e->y = y; e->x = x; e->tile = tile; e->attr = attr;
    }
    memcpy(wl2_oam_previous, snapshot, sizeof wl2_oam_previous);
    wl2_oam_previous_valid = 1;
}

static void wl2_trace_write_oam_events(void)
{
    if (!wl2_oam_path[0]) return;
    FILE *f = fopen(wl2_oam_path, "w");
    if (!f) {
        fprintf(stderr, "PipeClean: couldn't write Wario Land II OAM event trace at %s\n", wl2_oam_path);
        return;
    }
    fprintf(f, "PipeClean Wario Land II widescreen OAM visibility events v1\n");
    fprintf(f, "Game slot: %d  margins: left=%d right=%d  events: %zu  dropped: %u\n",
            wl2_trace_game, wl2_trace_left, wl2_trace_right, wl2_oam_event_count,
            (unsigned)wl2_oam_events_dropped);
    fprintf(f, "Regions: OFF=not rendered, LEFT/RIGHT=extra widescreen margins, NATIVE=original 160px viewport.\n");
    fprintf(f, "frame sprite from>to oldY oldX oldTile oldAttr newY newX newTile newAttr screenX screenY oldScreenX oldScreenY playerX playerY cameraX cameraY SCX SCY\n");
    for (size_t i = 0; i < wl2_oam_event_count; i++) {
        const Wl2OamEvent *e = &wl2_oam_events[i];
        fprintf(f, "%u %u %s>%s %02X %02X %02X %02X %02X %02X %02X %02X %d %d %d %d %04X %04X %02X %02X %02X %02X\n",
                (unsigned)e->frame, (unsigned)e->sprite,
                wl2_oam_region_name(e->from_region), wl2_oam_region_name(e->to_region),
                (unsigned)e->old_y, (unsigned)e->old_x, (unsigned)e->old_tile, (unsigned)e->old_attr,
                (unsigned)e->y, (unsigned)e->x, (unsigned)e->tile, (unsigned)e->attr,
                (int)e->sx, (int)e->sy, (int)e->old_sx, (int)e->old_sy,
                (unsigned)e->px, (unsigned)e->py, (unsigned)e->camx, (unsigned)e->camy,
                (unsigned)e->scx, (unsigned)e->scy);
    }
    fclose(f);
    fprintf(stderr, "PipeClean: wrote Wario Land II OAM event trace (%zu events, %u dropped) to %s\n",
            wl2_oam_event_count, (unsigned)wl2_oam_events_dropped, wl2_oam_path);
    wl2_oam_event_count = 0;
    wl2_oam_events_dropped = 0;
    wl2_oam_previous_valid = 0;
}

void wide_wl2_trace_finish(void)
{
    wide_wl2_trace_active = 0;
    wl2_trace_pending_valid = 0;
    wl2_trace_write_oam_events();
    if (!wl2_trace_count) return;

    Wl2TraceEntry *ordered[WL2_TRACE_SLOTS];
    size_t used = 0;
    for (size_t i = 0; i < WL2_TRACE_SLOTS; i++)
        if (wl2_trace_entries[i].used) ordered[used++] = &wl2_trace_entries[i];
    qsort(ordered, used, sizeof ordered[0], wl2_trace_hit_order);

    FILE *f = fopen(wl2_trace_path, "w");
    if (!f) {
        fprintf(stderr, "PipeClean: couldn't write Wario Land II widescreen trace at %s\n", wl2_trace_path);
        return;
    }
    fprintf(f, "PipeClean Wario Land II widescreen instruction trace v3\n");
    fprintf(f, "Title: %s\nROM CRC32: %08X\nROM size: %zu bytes\nCGB flag: %02X\n",
            wl2_trace_title, wl2_trace_crc, wl2_trace_rom_size, wl2_trace_cgb_flag);
    fprintf(f, "Game slot: %d  margins: left=%d right=%d  unique sites: %zu\n",
            wl2_trace_game, wl2_trace_left, wl2_trace_right, used);
    fprintf(f, "A/X/Y/camera columns show min-max across all hits at a site; CMP relation counts "
                "use unsigned A vs operand; branch taken/checks counts an adjacent JR/JP/CALL/RET cc.\n");
    fprintf(f, "hits first last bank pc romoff op code8 operand A[min,max] O[min,max] "
                "CMP[lt,eq,gt] BR[taken,checks] BC[min,max] DE[min,max] HL[min,max] "
                "X[min,max] Y[min,max] SCX[min,max] SCY[min,max] CAMX[min,max] CAMY[min,max]\n");
    for (size_t i = 0; i < used; i++) {
        const Wl2TraceEntry *e = ordered[i];
        fprintf(f, "%u %u %u %u %04X %06X %02X ",
                (unsigned)e->hits, (unsigned)e->first_frame, (unsigned)e->last_frame,
                (unsigned)(e->rom_offset >> 14), (unsigned)e->pc, (unsigned)e->rom_offset,
                (unsigned)e->opcode);
        for (unsigned byte = 0; byte < sizeof e->code; byte++)
            fprintf(f, "%02X", (unsigned)e->code[byte]);
        fprintf(f, " %02X %02X-%02X %02X-%02X %u,%u,%u %u,%u "
                    "%04X-%04X %04X-%04X %04X-%04X "
                    "%04X-%04X %04X-%04X %02X-%02X %02X-%02X %02X-%02X %02X-%02X\n",
                (unsigned)e->operand, (unsigned)e->a_min, (unsigned)e->a_max,
                (unsigned)e->operand_min, (unsigned)e->operand_max,
                (unsigned)e->cmp_lt, (unsigned)e->cmp_eq, (unsigned)e->cmp_gt,
                (unsigned)e->branch_taken, (unsigned)e->branch_checks,
                (unsigned)e->bc_min, (unsigned)e->bc_max, (unsigned)e->de_min, (unsigned)e->de_max,
                (unsigned)e->hl_min, (unsigned)e->hl_max, (unsigned)e->px_min, (unsigned)e->px_max,
                (unsigned)e->py_min, (unsigned)e->py_max, (unsigned)e->scx_min, (unsigned)e->scx_max,
                (unsigned)e->scy_min, (unsigned)e->scy_max, (unsigned)e->camx_min, (unsigned)e->camx_max,
                (unsigned)e->camy_min, (unsigned)e->camy_max);
    }
    fclose(f);
    fprintf(stderr, "PipeClean: wrote Wario Land II widescreen trace (%zu sites) to %s\n",
            used, wl2_trace_path);
    wl2_trace_count = 0;
    memset(wl2_trace_entries, 0, sizeof wl2_trace_entries);
}

static size_t wl2_trace_header_rom_size(void)
{
    if (!rom) return 0;
    unsigned code = rom[0x148];
    if (code <= 8) return (size_t)0x8000u << code;
    switch (code) {
    case 0x52: return (size_t)72 * 0x4000u;
    case 0x53: return (size_t)80 * 0x4000u;
    case 0x54: return (size_t)96 * 0x4000u;
    default: return 0;
    }
}

static uint32_t wl2_trace_rom_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static void wl2_trace_begin(int game, int left, int right)
{
    wl2_trace_pending_valid = 0;
    memset(wl2_trace_entries, 0, sizeof wl2_trace_entries);
    wl2_trace_count = 0;
    wl2_oam_event_count = 0;
    wl2_oam_events_dropped = 0;
    wl2_oam_previous_valid = 0;
    memset(wl2_oam_previous, 0, sizeof wl2_oam_previous);
    wl2_trace_game = game;
    wl2_trace_left = left;
    wl2_trace_right = right;
    wl2_trace_rom_size = wl2_trace_header_rom_size();
    wl2_trace_crc = wl2_trace_rom_size ? wl2_trace_rom_crc32(rom, wl2_trace_rom_size) : 0;
    wl2_trace_cgb_flag = rom ? rom[0x143] : 0;
    memset(wl2_trace_title, 0, sizeof wl2_trace_title);
    if (rom) {
        memcpy(wl2_trace_title, rom + 0x134, 16);
        for (size_t i = 0; i < 16; i++) {
            unsigned char c = (unsigned char)wl2_trace_title[i];
            if (c < 32 || c > 126) { wl2_trace_title[i] = 0; break; }
        }
    } else {
        snprintf(wl2_trace_title, sizeof wl2_trace_title, "%s", "unknown");
    }
    if (!wl2_trace_atexit_registered) {
        if (atexit(wide_wl2_trace_finish) == 0) wl2_trace_atexit_registered = 1;
    }
    wide_wl2_trace_active = 1;
}

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

static int text_has_dx(const char *s)
{
    if (!s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (tolower(p[0]) == 'd' && p[1] && tolower(p[1]) == 'x') return 1;
    }
    return 0;
}

int wide_is_wario_dx_profile(const char *title, const char *path)
{
    /* This DX IPS expands the clean 512 KiB cart to 1 MiB and changes
     * the header to CGB-only MBC5. Header-based recognition also works when
     * the patch is loaded under an arbitrary filename and keeps this module
     * independent of cartridge metadata for isolated tests. */
    if (rom && rom[0x143] == 0xC0 &&
        rom[0x147] == 0x1B && rom[0x148] == 0x05)
        return 1;
    return text_has_dx(title) || text_has_dx(path);
}

void wide_dims_profile(int game, int pct, int wario_dx, int *l, int *r)
{
    *l = *r = 0;
    if (pct <= 0) return;
    if (game == GAME_WARIO_SML3) {
        /* Wario Land: SML3 has separately verified clean/DX profiles. */
        int margin = (wario_dx ? 30 : 33) * pct;
        margin = (margin + 50) / 100;
        *l = *r = margin;
        return;
    }
    if (game == GAME_WARIO_LAND3_GBC) {
        /* Start conservatively at 32 pixels per side; runtime traces will
         * identify which level/object windows need game-specific treatment. */
        int margin = (32 * pct + 50) / 100;
        *l = *r = margin;
        return;
    }
    if (game == GAME_WARIO_LAND2_GB || game == GAME_WARIO_LAND2_GBC) {
        /* WL2 currently uses the emulator's generic scroll/tilemap margin
         * rendering. Keep the exposed extension inside the 256px BG ring;
         * do not patch game code until per-version spawn bounds are verified. */
        int margin = (32 * pct + 50) / 100;
        *l = *r = margin;
        return;
    }
    if (game == GAME_SML2) { *l = (49 * pct + 50) / 100; *r = (33 * pct + 50) / 100; return; }
    if (game == GAME_SML) {
        int rr = (56 * pct + 50) / 100, ll = (32 * pct + 50) / 100;
        int k = sml_k(rr), lm = sml_lmax(k);
        *l = ll < lm ? ll : lm; *r = rr;
    }
}

void wide_dims(int game, int pct, int *l, int *r)
{
    wide_dims_profile(game, pct, 0, l, r);
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

/* SML2's decompressed 16x16-block level is live in cart RAM and WRAM.
 * Compose only the outer margins from it; the native 160px centre remains
 * the real PPU framebuffer. Validate the native tilemap before drawing. */
static int sml2_compositor_installed;
static int sml2_compositor_frame_ok;
static int sml2_compositor_homeworld;
static int sml2_compositor_left, sml2_compositor_right;
static int sml2_compositor_world_x0, sml2_compositor_world_y0;
#define SML2_CAPTURED_SPRITE_CAP 1024
#define SML2_MAX_PIECES 64
static WideSml2Sprite sml2_actor_build[SML2_CAPTURED_SPRITE_CAP];
static WideSml2Sprite sml2_actor_frame[SML2_CAPTURED_SPRITE_CAP];
static int sml2_actor_build_count, sml2_actor_frame_count;
static int sml2_read16(uint16_t a);

#define SML2_MAP_BASE        0xB000u
#define SML2_BLOCKDEF_BASE   0xA600u
#define SML2_MAP_WIDTH_PX    4096
#define SML2_MAP_HEIGHT_PX   768
#define SML2_MAP_COLS        256
#define SML2_MAP_MAX_BLOCK   0x7Fu
#define SML2_CAMERA_X        0xFFCAu
#define SML2_CAMERA_Y        0xFFC8u
#define SML2_CAMERA_CENTRE_X 80
#define SML2_CAMERA_CENTRE_Y 72

/* The Homeworld is a 64x64 grid of raw 8x8 tile IDs assembled from four
 * 32x32 quadrants. Rows 0-31 occupy $B000-$B7FF and rows 32-63 occupy
 * $B800-$BFFF; each row is 64 bytes wide. Its camera is at $A680/$A682. */
#define SML2_HOME_MAP_BASE   0xB000u
#define SML2_HOME_MAP_COLS   64
#define SML2_HOME_MAP_ROWS   64
#define SML2_HOME_MAP_WIDTH  (SML2_HOME_MAP_COLS * 8)
#define SML2_HOME_MAP_HEIGHT (SML2_HOME_MAP_ROWS * 8)
#define SML2_HOME_CAMERA_X   0xA680u
#define SML2_HOME_CAMERA_Y   0xA682u
#define SML2_HOME_HUD_WX     80
#define SML2_MODE_ADDR       0xFF9Bu
#define SML2_BONUS_ADDR      0xA28Bu
#define SML2_MODE_PLAY       0x04u
#define SML2_MODE_PAUSE      0x08u
#define SML2_MODE_DEATH      0x09u
#define SML2_HUD_WY          0x88
#define SML2_HUD_WX          7

/* Resolve an absolute world pixel to the tile index assigned by the live
 * decompressed block map. Each 16x16 block contains four 8x8 tile indices. */
int wide_sml2_tile_at(int world_x, int world_y, uint8_t *tile)
{
    if (!sml2_compositor_installed || !tile ||
        world_x < 0 || world_x >= SML2_MAP_WIDTH_PX ||
        world_y < 0 || world_y >= SML2_MAP_HEIGHT_PX)
        return 0;

    unsigned block_x = (unsigned)world_x >> 4;
    unsigned block_y = (unsigned)world_y >> 4;
    unsigned map_address = SML2_MAP_BASE + block_y * SML2_MAP_COLS + block_x;
    unsigned block = rd8((uint16_t)map_address);
    if (block > SML2_MAP_MAX_BLOCK) return 0;

    unsigned quadrant = ((((unsigned)world_y >> 3) & 1u) << 1) |
                         (((unsigned)world_x >> 3) & 1u);
    *tile = rd8((uint16_t)(SML2_BLOCKDEF_BASE + block * 4u + quadrant));
    return 1;
}

/* This exact LCDC/window setup is used by SML2's Homeworld world map. */
int wide_sml2_homeworld_active(int lcdc, int wx, int wy)
{
    return sml2_compositor_installed && lcdc == 0xE3 &&
           wx == SML2_HOME_HUD_WX && wy == SML2_HUD_WY;
}

/* Allocate the Homeworld's fixed extra width toward the map edges. The native
 * 160px view starts at screen_left; keep all 512 output pixels inside the
 * 512px map whenever the camera position allows it. */
void wide_sml2_homeworld_dims(int screen_left, int extra_total,
                              int *left, int *right)
{
    if (!left || !right) return;
    if (extra_total < 0) extra_total = 0;

    int left_capacity = screen_left;
    int right_capacity = SML2_HOME_MAP_WIDTH - screen_left - 160;
    if (left_capacity < 0) left_capacity = 0;
    if (right_capacity < 0) right_capacity = 0;
    if (left_capacity > extra_total) left_capacity = extra_total;
    if (right_capacity > extra_total) right_capacity = extra_total;

    /* The extra canvas must fit between the map's left and right edges. */
    int min_left = extra_total - right_capacity;
    int max_left = left_capacity;
    if (min_left < 0) min_left = 0;
    if (max_left > extra_total) max_left = extra_total;
    if (min_left > max_left) {
        /* Defensive fallback for a camera coordinate outside the map. */
        *left = left_capacity;
        *right = extra_total - *left;
        return;
    }

    int l = extra_total / 2;
    if (l < min_left) l = min_left;
    if (l > max_left) l = max_left;
    *left = l;
    *right = extra_total - l;
}

/* Read and align the live Homeworld camera here, where emulator memory access
 * is available. The PPU keeps this as an optional compositor hook. */
void wide_sml2_homeworld_dims_from_camera(int scx, int extra_total,
                                          int *left, int *right)
{
    if (!left || !right) return;
    int screen_left = sml2_read16(SML2_HOME_CAMERA_X);
    screen_left += (int8_t)(uint8_t)(scx - (screen_left & 0xFF));
    wide_sml2_homeworld_dims(screen_left, extra_total, left, right);
}

/* Return the live Homeworld camera origin, aligned to the PPU scroll registers.
 * The overview renderer uses it only to position the currently animated player
 * and other live OAM sprites over the full static map. */
int wide_sml2_homeworld_camera(int scx, int scy, int *camera_x, int *camera_y)
{
    if (!camera_x || !camera_y || !sml2_compositor_installed) return 0;
    int x = sml2_read16(SML2_HOME_CAMERA_X);
    int y = sml2_read16(SML2_HOME_CAMERA_Y);
    x += (int8_t)(uint8_t)(scx - (x & 0xFF));
    y += (int8_t)(uint8_t)(scy - (y & 0xFF));
    *camera_x = x;
    *camera_y = y;
    return 1;
}

/* Homeworld's full map has 64 rows of 64 tile IDs. The row stride crosses
 * from $B000 into $B800 at row 32, covering all four 32x32 quadrants. */
int wide_sml2_homeworld_tile_at(int world_x, int world_y, uint8_t *tile)
{
    if (!sml2_compositor_installed || !tile ||
        world_x < 0 || world_x >= SML2_HOME_MAP_WIDTH ||
        world_y < 0 || world_y >= SML2_HOME_MAP_HEIGHT)
        return 0;

    unsigned tile_x = (unsigned)world_x >> 3;
    unsigned tile_y = (unsigned)world_y >> 3;
    unsigned address = SML2_HOME_MAP_BASE + tile_y * 0x40u + tile_x;
    *tile = rd8((uint16_t)address);
    return 1;
}

/* Prepare a per-frame world transform, then compare the decoded source map
 * against the real PPU tilemap. Unsupported scenes fail closed. */
void wide_sml2_prepare_frame(int left, int right, int scx, int scy,
                             int lcdc, int wx, int wy, const uint8_t *tilemap)
{
    sml2_compositor_frame_ok = 0;
    sml2_compositor_homeworld = 0;
    sml2_compositor_left = left;
    sml2_compositor_right = right;
    if (!sml2_compositor_installed || !tilemap ||
        left + right <= 0 || left < 0 || right < 0)
        return;

    /* On the Homeworld map the game selects WX=80 and WY=136. Its full
       512x512px tilemap occupies SRAM $B000-$BFFF as four 32x32 quadrants,
       not the level's 4096x768px 16x16-block map. */
    if (wide_sml2_homeworld_active(lcdc, wx, wy)) {
        int camera_x = sml2_read16(SML2_HOME_CAMERA_X);
        int camera_y = sml2_read16(SML2_HOME_CAMERA_Y);
        int screen_left = camera_x;
        int world_top = camera_y;
        screen_left += (int8_t)(uint8_t)(scx - (screen_left & 0xFF));
        world_top += (int8_t)(uint8_t)(scy - (world_top & 0xFF));

        int hits = 0, samples = 0;
        int first_x = (8 - (scx & 7)) & 7;
        int first_y = (8 - (scy & 7)) & 7;
        int map_off = (lcdc & 0x08) ? 0x1C00 : 0x1800;
        for (int sy = first_y; sy < 136; sy += 8) {
            for (int sx = first_x; sx < 160; sx += 8) {
                uint8_t decoded;
                if (!wide_sml2_homeworld_tile_at(screen_left + sx, world_top + sy,
                                           &decoded))
                    return;
                unsigned map_index = (unsigned)map_off +
                    (unsigned)(((sy + scy) >> 3) & 31) * 32u +
                    (unsigned)(((sx + scx) >> 3) & 31);
                if (decoded == tilemap[map_index]) hits++;
                samples++;
            }
        }
        /* Keep the same fail-closed validation as regular levels, since some
           ROM revisions stamp a small number of tiles straight into VRAM. */
        if (!samples || hits * 100 < samples * 95) return;
        sml2_compositor_world_x0 = screen_left - left;
        sml2_compositor_world_y0 = world_top;
        sml2_compositor_homeworld = 1;
        sml2_compositor_frame_ok = 1;
        return;
    }

    unsigned mode = rd8(SML2_MODE_ADDR);
    if (mode != SML2_MODE_PLAY && mode != SML2_MODE_PAUSE && mode != SML2_MODE_DEATH)
        return;
    if (rd8(SML2_BONUS_ADDR) & 0xF0u) return;
    if (lcdc != 0xE3 || wx != SML2_HUD_WX || wy != SML2_HUD_WY)
        return;

    int cam_x = sml2_read16(SML2_CAMERA_X);
    int cam_y = sml2_read16(SML2_CAMERA_Y);
    int screen_left = cam_x - SML2_CAMERA_CENTRE_X;
    /* Align to the live scroll registers, including the game's vertical shake. */
    screen_left += (int8_t)(uint8_t)(scx - (screen_left & 0xFF));
    int world_top = cam_y - SML2_CAMERA_CENTRE_Y;
    world_top += (int8_t)(uint8_t)(scy - (world_top & 0xFF));
    sml2_compositor_world_x0 = screen_left - left;
    sml2_compositor_world_y0 = world_top;

    int hits = 0, samples = 0;
    int first_x = (8 - (scx & 7)) & 7;
    int first_y = (8 - (scy & 7)) & 7;
    int map_off = (lcdc & 0x08) ? 0x1C00 : 0x1800;
    for (int sy = first_y; sy < 136; sy += 8) {
        for (int sx = first_x; sx < 160; sx += 8) {
            uint8_t decoded;
            if (!wide_sml2_tile_at(screen_left + sx, world_top + sy, &decoded))
                return;
            unsigned map_index = (unsigned)map_off +
                (unsigned)(((sy + scy) >> 3) & 31) * 32u +
                (unsigned)(((sx + scx) >> 3) & 31);
            if (decoded == tilemap[map_index]) hits++;
            samples++;
        }
    }
    /* Some blocks are stamped directly into VRAM by the ROM; tolerate those
       known exceptions while rejecting unrelated RAM or another scene. */
    if (!samples || hits * 100 < samples * 95) return;
    sml2_compositor_frame_ok = 1;
}

/* Return 1 for a valid world tile, -1 for a margin pixel outside the level,
 * and 0 when this frame/pixel should use the ordinary PPU output. */
int wide_sml2_margin_tile(int screen_x, int screen_y, uint8_t *tile,
                         int *world_x, int *world_y)
{
    if (!sml2_compositor_frame_ok || !tile || !world_x || !world_y ||
        screen_y < 0 || screen_y >= 136 ||
        screen_x < 0 ||
        screen_x >= sml2_compositor_left + 160 + sml2_compositor_right ||
        (screen_x >= sml2_compositor_left &&
         screen_x < sml2_compositor_left + 160))
        return 0;
    int x = sml2_compositor_world_x0 + screen_x;
    int y = sml2_compositor_world_y0 + screen_y;
    *world_x = x;
    *world_y = y;
    if (sml2_compositor_homeworld) {
        if (!wide_sml2_homeworld_tile_at(x, y, tile)) return -1;
    } else if (!wide_sml2_tile_at(x, y, tile)) {
        return -1;
    }
    return 1;
}


static uint8_t sml2_mapped_rom_byte(unsigned address)
{
    if (address < 0x4000u || address >= 0x8000u || !cart_hi) return 0xFF;
    return cart_hi[address - 0x4000u];
}

static int sml2_mapped_bank(void)
{
    if (!rom || !cart_hi || (uintptr_t)cart_hi < (uintptr_t)rom) return -1;
    uintptr_t offset = (uintptr_t)cart_hi - (uintptr_t)rom;
    if ((offset & 0x3FFFu) != 0) return -1;
    return (int)(offset >> 14);
}

static int sml2_is_actor_bank(int bank)
{
    return bank == 3 || bank == 35 || bank == 40 || bank == 58;
}

static int sml2_is_emitter_bank(int bank)
{
    return bank == 1 || bank == 0x2C || bank == 0x2D;
}

static void sml2_capture_actor(uint8_t sentinel)
{
    if (!rom || !sml2_is_actor_bank(sml2_mapped_bank()) || sentinel == 0x80u) return;
    int ax = (rd8(0xFFD0u) << 8) | rd8(0xFFD1u);
    int ay = (rd8(0xFFD3u) << 8) | rd8(0xFFD4u);
    unsigned idx = rd8(0xFFDBu);
    uint8_t flip = (uint8_t)(rd8(0xFFDCu) ^ rd8(0xFFDDu) ^ rd8(0xFFDEu));
    unsigned table = rd8(0xAF06u) ? 0x4F11u : 0x40B1u;
    unsigned entry = table + idx * 2u;
    unsigned de = (unsigned)sml2_mapped_rom_byte(entry) |
                  ((unsigned)sml2_mapped_rom_byte(entry + 1u) << 8);
    if (de < 0x4000u || de >= 0x8000u) return;
    for (int n = 0; n < SML2_MAX_PIECES && de + 3u < 0x8000u; n++, de += 4u) {
        uint8_t yraw = sml2_mapped_rom_byte(de);
        if (yraw == 0x80u) break;
        uint8_t xraw = sml2_mapped_rom_byte(de + 1u);
        int yoff = (flip & 0x40u) ? (int8_t)(uint8_t)((~yraw & 0xFFu) - 7u) : (int8_t)yraw;
        int xoff = (flip & 0x20u) ? (int8_t)(uint8_t)((~xraw & 0xFFu) - 7u) : (int8_t)xraw;
        if (sml2_actor_build_count >= SML2_CAPTURED_SPRITE_CAP) break;
        WideSml2Sprite *sp = &sml2_actor_build[sml2_actor_build_count++];
        sp->x = (int16_t)(ax + xoff);
        sp->y = (int16_t)(ay + yoff);
        sp->tile = sml2_mapped_rom_byte(de + 2u);
        sp->attr = (uint8_t)(sml2_mapped_rom_byte(de + 3u) ^ flip);
    }
}


static int sml2_fireball_world_match(unsigned idx, unsigned sx, unsigned sy,
                                     int *world_x, int *world_y)
{
    if (idx < 0xB0u || idx > 0xB7u) return 0;
    int cam_x = sml2_read16(SML2_CAMERA_X);
    int cam_y = sml2_read16(SML2_CAMERA_Y);
    for (int i = 0; i < 2; i++) {
        unsigned base = 0xA880u + (unsigned)i * 0x10u;
        if (!rd8((uint16_t)base)) continue;
        int y = rd8((uint16_t)(base + 1u)) | (rd8((uint16_t)(base + 2u)) << 8);
        int x = rd8((uint16_t)(base + 3u)) | (rd8((uint16_t)(base + 4u)) << 8);
        if (((unsigned)(x - cam_x + 80) & 0xFFu) != sx) continue;
        if (((unsigned)(y - cam_y + 70) & 0xFFu) != sy) continue;
        if (world_x) *world_x = x;
        if (world_y) *world_y = y;
        return 1;
    }
    return 0;
}

/* Bank 1's shared emitter draws Mario's projectiles. Take the sampled screen-X
 * as an argument so the tap does not recursively read $FFC5. */
static void sml2_capture_emitter(unsigned sx)
{
    unsigned sy = rd8(0xFFC4u);
    unsigned idx = rd8(0xFFC6u);
    int pal = rd8(0xFFC7u) != 0;

    int cam_x = sml2_read16(SML2_CAMERA_X);
    int cam_y = sml2_read16(SML2_CAMERA_Y);
    int native_left = cam_x - SML2_CAMERA_CENTRE_X;
    int native_top = cam_y - SML2_CAMERA_CENTRE_Y;
    int scx = rd8(0xFF43u), scy = rd8(0xFF42u);
    native_left += (int8_t)(uint8_t)(scx - (native_left & 0xFF));
    native_top += (int8_t)(uint8_t)(scy - (native_top & 0xFF));

    int ox = native_left + (sx >= 0xD0u ? (int)sx - 256 : (int)sx) - 8;
    int oy = native_top + (sy >= 0xD0u ? (int)sy - 256 : (int)sy) - 16;
    int fireball_x, fireball_y;
    if (sml2_fireball_world_match(idx, sx, sy, &fireball_x, &fireball_y)) {
        ox = fireball_x - 8;
        oy = fireball_y - 16;
    }

    unsigned entry = 0x4000u + idx * 2u;
    unsigned de = (unsigned)sml2_mapped_rom_byte(entry) |
                  ((unsigned)sml2_mapped_rom_byte(entry + 1u) << 8);
    if (de < 0x4000u || de >= 0x8000u) return;
    for (int n = 0; n < SML2_MAX_PIECES && de + 3u < 0x8000u; n++, de += 4u) {
        uint8_t yraw = sml2_mapped_rom_byte(de);
        if (yraw == 0x80u) break;
        if (sml2_actor_build_count >= SML2_CAPTURED_SPRITE_CAP) break;
        WideSml2Sprite *sp = &sml2_actor_build[sml2_actor_build_count++];
        sp->x = (int16_t)(ox + (int8_t)sml2_mapped_rom_byte(de + 1u));
        sp->y = (int16_t)(oy + (int8_t)yraw);
        sp->tile = sml2_mapped_rom_byte(de + 2u);
        sp->attr = sml2_mapped_rom_byte(de + 3u);
        if (pal) sp->attr |= 0x10u;
    }
}

uint8_t wide_sml2_read_tap(uint16_t address, uint8_t value)
{
    if (!sml2_compositor_installed || !rom || !cart_hi) return value;
    int bank = sml2_mapped_bank();
    if (address == 0xFFE2u && sml2_is_actor_bank(bank) &&
        (cpu.pc == 0x4019u || cpu.pc == 0x401Bu)) {
        sml2_capture_actor(value);
    } else if (address == 0xFFC5u && sml2_is_emitter_bank(bank)) {
        static const uint16_t tap_pcs[] = {0x52BAu, 0x5E5Du, 0x5D8Bu, 0x5E3Fu};
        for (unsigned i = 0; i < sizeof tap_pcs / sizeof tap_pcs[0]; i++) {
            if (cpu.pc == tap_pcs[i] || cpu.pc == (uint16_t)(tap_pcs[i] - 2u)) {
                sml2_capture_emitter(value);
                break;
            }
        }
    }
    return value;
}

void wide_sml2_snapshot_actors(void)
{
    sml2_actor_frame_count = sml2_actor_build_count;
    if (sml2_actor_frame_count)
        memcpy(sml2_actor_frame, sml2_actor_build,
               (size_t)sml2_actor_frame_count * sizeof sml2_actor_frame[0]);
    sml2_actor_build_count = 0;
}

const WideSml2Sprite *wide_sml2_actor_sprites(int *count)
{
    if (count) *count = sml2_compositor_frame_ok ? sml2_actor_frame_count : 0;
    return sml2_compositor_frame_ok && sml2_actor_frame_count
        ? sml2_actor_frame : NULL;
}

int wide_sml2_world_origin(int *world_x, int *world_y)
{
    if (!sml2_compositor_frame_ok || !world_x || !world_y) return 0;
    *world_x = sml2_compositor_world_x0;
    *world_y = sml2_compositor_world_y0;
    return 1;
}


static int sml2_read16(uint16_t a)
{
    return (int)rd8(a) | ((int)rd8((uint16_t)(a + 1)) << 8);
}

static int sml2_scan_value(int side)
{
    int cam = sml2_read16(0xFFCA);
    int vanilla = side == 0 ? cam + 112 : cam - 112;
    int target = side == 0 ? vanilla + sml2_right_extra : vanilla - sml2_left_extra;
    /* The decompressed SML2 world is at most 256 blocks (4096 pixels) wide.
       Never let the widened scanner walk beyond the level's coordinate space. */
    if (target < 0) target = 0;
    if (target > SML2_MAP_WIDTH_PX) target = SML2_MAP_WIDTH_PX;

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
    if ((!sml2_right_extra && !sml2_left_extra) || !rom || !cart_hi)
        return value;

    /* Widened activation keeps actors alive outside the native 160px strip.
     * The stock draw routine computes screen X modulo 256, so an actor beyond
     * that strip can wrap back into the centre as a ghost. At the verified
     * $409B/$409E read of $A2B1, give that routine a scroll shadow that makes
     * its own whole-actor cull reject only actors the native strip cannot see.
     * Their separately captured world-space sprites are still drawn in margins. */
    if (!sml2_compositor_homeworld && address == 0xA2B1u &&
        sml2_is_actor_bank(sml2_mapped_bank()) &&
        (cpu.pc == 0x409Bu || cpu.pc == 0x409Eu)) {
        int actor_x = (rd8(0xFFD0u) << 8) | rd8(0xFFD1u);
        int cam_x = sml2_read16(SML2_CAMERA_X);
        int native_left = cam_x - SML2_CAMERA_CENTRE_X;
        int scx = rd8(0xFF43u);
        native_left += (int8_t)(uint8_t)(scx - (native_left & 0xFF));
        int rel = actor_x - native_left;
        if (rel < -8 || rel >= 176)
            return (uint8_t)((rd8(0xFFD1u) + 8u - 0xB8u) & 0xFFu);
    }

    /* The activation/cull/scanner hooks below target the bank-2 world builder.
     * Do not apply those overrides while a different ROM bank is mapped. */
    if (cart_hi != rom + 0x8000) return value;

    /* The entity-loader rebuilds both its activation and cull windows from
     * the camera every frame. These are RAM-backed bounds, so widening the
     * final ADD HL,DE alone leaves the next window narrower and causes actors
     * to disappear before they reach the visible margin. Override the four
     * bytes at the ROM0 memcpy call site, using the exact same camera-relative
     * bounds the hardware logic uses. */
    if (cpu.pc == 0x3CAA || cpu.pc == 0x3CAB) {
        int cam = sml2_read16(0xFFCA);
        int upper = cam + 0x60 + sml2_right_extra;
        int lower = cam - 0x60 - sml2_left_extra;
        int cull = address >= 0xAF1A && address <= 0xAF1D;
        if (cull) {
            upper = cam + 0xA0 + sml2_right_extra;
            lower = cam - 0xA0 - sml2_left_extra;
        }
        /* Bounds are unsigned 16-bit world coordinates, but the beginning/end
         * of a level is not a wraparound point. Saturate rather than letting a
         * negative left bound become $FFxx and make nearby actors look culled. */
        if (lower < 0) lower = 0;
        if (upper > 0xFFFF) upper = 0xFFFF;
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
    /* These are runtime scanner/compositor latches, not emulated RAM. A state
     * load can jump to another room, so never reuse a stale frame transform. */
    sml2_scan_edge[0] = sml2_scan_edge[1] = -1;
    sml2_scan_pick[0] = sml2_scan_pick[1] = 0;
    sml2_scan_frame[0] = sml2_scan_frame[1] = -1;
    sml2_compositor_frame_ok = 0;
    sml2_compositor_homeworld = 0;
    sml2_actor_build_count = sml2_actor_frame_count = 0;
}

/* The DX reference patch widens one additional active-range operand.
 * This is LD BC,$00D0 at ROM offset $31FD; its little-endian low operand is
 * at $31FE. Check the whole instruction before touching it. */
static void wario_patch_dx_activation_operand(void)
{
    if (rom[0x31FD] == 0x01 &&
        rom[0x31FE] == 0xD0 &&
        rom[0x31FF] == 0x00) {
        rom[0x31FE] = 0xD1;
    }
}


/* Wario Land 3's object manager has its own fixed-width camera checks.
 * Discover the checked instruction sequences from the loaded ROM rather than
 * hard-coding addresses for one banking/revision layout. The ROM is only read;
 * changes happen in the interpreter's live CPU state. */
#define WL3_MAX_BOUND_HOOKS 16u
#define WL3_MAX_WIDTH_HOOKS 16u
#define WL3_MAX_LEFT_CANDIDATES 64u
#define WL3_MAX_EDGE_HOOKS 16u
#define WL3_MAX_LOOP_HOOKS 8u

static int wl3_profile_active;
static int wl3_left_extra, wl3_right_extra;
static size_t wl3_rom_size;
static uint16_t wl3_camera_x_addr;
static uint16_t wl3_room_tiles_addr;
static size_t wl3_bound_left_hooks[WL3_MAX_BOUND_HOOKS];
static size_t wl3_bound_left_count;
static size_t wl3_bound_width_hooks[WL3_MAX_WIDTH_HOOKS];
static size_t wl3_bound_width_count;
static size_t wl3_right_spawn_adds[WL3_MAX_EDGE_HOOKS];
static size_t wl3_right_spawn_count;
static size_t wl3_left_spawn_adds[WL3_MAX_EDGE_HOOKS];
static size_t wl3_left_spawn_count;
static size_t wl3_col_loop_loads[WL3_MAX_LOOP_HOOKS];
static size_t wl3_col_loop_count;

static uint16_t wl3_pc_for_rom_offset(size_t off)
{
    return off < 0x4000u ? (uint16_t)off
                         : (uint16_t)(0x4000u + (off & 0x3FFFu));
}

static int wl3_rom_offset_is_mapped(size_t off, uint16_t pc)
{
    if (!rom || pc != wl3_pc_for_rom_offset(off)) return 0;
    if (off < 0x4000u) return cart_lo == rom;
    return cart_hi && cart_hi == rom + (off & ~(size_t)0x3FFFu);
}

static int wl3_bounds_signature(const uint8_t *p)
{
    /* Horizontal half of CheckObjectOnScreen and its later actor-despawn
     * check. It loads wCameraSCX, then tests E + 24 (or E + $6f) against it.
     * This is deliberately a local signature so bank/link-address shifts do
     * not break detection. The width-add is found separately in the following
     * control-flow window. */
    return p[0] == 0x21 && p[3] == 0x2A && p[4] == 0x6E && p[5] == 0x67 &&
           p[6] == 0x7B && p[7] == 0xC6 &&
           (p[8] == 0x18 || p[8] == 0x6F) && p[9] == 0x5F &&
           p[10] == 0x30;
}

static int wl3_right_spawn_signature(const uint8_t *p)
{
    /* ScrollCameraRight's object-row look-ahead: camera X + $b0 (176 px). */
    return p[0] == 0x21 && p[3] == 0x3A && p[4] == 0xE6 && p[5] == 0xF8 &&
           p[6] == 0x5F && p[7] == 0x66 && p[8] == 0x6B &&
           p[9] == 0x11 && p[10] == 0xB0 && p[11] == 0x00 &&
           p[12] == 0x19 && p[13] == 0x7C && p[14] == 0xFE &&
           p[15] == 0x0A && p[16] == 0x38 && p[18] == 0x21 &&
           p[19] == 0xF8 && p[20] == 0x09 && p[21] == 0x7C;
}

static int wl3_left_spawn_signature(const uint8_t *p)
{
    /* Horizontal/vertical edge setup: camera X - $10 (one block of lead). */
    return p[0] == 0x21 && p[3] == 0x3A && p[4] == 0xE6 && p[5] == 0xF8 &&
           p[6] == 0x5F && p[7] == 0x66 && p[8] == 0x6B &&
           p[9] == 0x11 && p[10] == 0xF0 && p[11] == 0xFF &&
           p[12] == 0x19 && p[13] == 0xCB && p[14] == 0x7C &&
           p[15] == 0x28 && p[17] == 0x21 && p[18] == 0x00 &&
           p[19] == 0x00;
}

static void wl3_scan_runtime_hooks(size_t rom_size)
{
    size_t left_candidates[WL3_MAX_LEFT_CANDIDATES];
    size_t left_candidate_count = 0;
    wl3_rom_size = rom_size;
    wl3_camera_x_addr = 0;
    wl3_room_tiles_addr = 0;
    wl3_bound_left_count = wl3_bound_width_count = 0;
    wl3_right_spawn_count = wl3_left_spawn_count = wl3_col_loop_count = 0;
    if (!rom || rom_size < 30u) return;

    for (size_t off = 0; off + 11u <= rom_size; ++off) {
        if ((off >> 14) != ((off + 10u) >> 14)) continue;
        const uint8_t *p = rom + off;
        if (wl3_bounds_signature(p)) {
            if (left_candidate_count < WL3_MAX_LEFT_CANDIDATES)
                left_candidates[left_candidate_count++] = off;
            /* LD HL,wCameraSCX. The game stores this coordinate high byte
             * first, and its low byte tracks the hardware SCX register. */
            uint16_t camera = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
            if (!wl3_camera_x_addr && camera >= 0xC000u && camera < 0xD000u)
                wl3_camera_x_addr = camera;
        }
    }

    /* Func_298d loads the room-specific 0x200-byte 16x16-block -> 2x2-tile
     * table with LD DE,wRoomBlockTiles; LD BC,$0200; CALL CopyHLToDE_BC.
     * Detect its RAM destination from code instead of baking in a WRAM layout. */
    for (size_t off = 0; off + 7u <= rom_size; ++off) {
        if ((off >> 14) != ((off + 6u) >> 14)) continue;
        const uint8_t *p = rom + off;
        if (p[0] != 0x11u || p[3] != 0x01u || p[4] != 0x00u ||
            p[5] != 0x02u || p[6] != 0xCDu) continue;
        uint16_t dest = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
        if (dest >= 0xC000u && dest + 0x200u <= 0xCC00u) {
            wl3_room_tiles_addr = dest;
            break;
        }
    }

    /* The game repeats the horizontal camera test for draw visibility and
     * despawn. Its right-side width operand is in the same basic block chain,
     * after the left-edge compare. Associate the nearest following 7D C6 A0
     * with each signature, rather than assuming a fixed full-routine offset. */
    for (size_t i = 0; i < left_candidate_count; ++i) {
        size_t off = left_candidates[i];
        if (wl3_bound_left_count < WL3_MAX_BOUND_HOOKS)
            wl3_bound_left_hooks[wl3_bound_left_count++] = off + 6u;
        size_t end = off + 128u;
        if (end > rom_size) end = rom_size;
        if ((off >> 14) != ((end - 1u) >> 14)) end = ((off >> 14) + 1u) << 14;
        for (size_t q = off + 12u; q + 2u < end; ++q) {
            if (rom[q] == 0x7Du && rom[q + 1u] == 0xC6u && rom[q + 2u] == 0xA0u) {
                if (wl3_bound_width_count < WL3_MAX_WIDTH_HOOKS)
                    wl3_bound_width_hooks[wl3_bound_width_count++] = q + 1u;
                break;
            }
        }
    }

    for (size_t off = 0; off + 22u <= rom_size; ++off) {
        if ((off >> 14) != ((off + 21u) >> 14)) continue;
        if (wl3_right_spawn_signature(rom + off) &&
            wl3_right_spawn_count < WL3_MAX_EDGE_HOOKS)
            wl3_right_spawn_adds[wl3_right_spawn_count++] = off + 12u;
    }

    /* Func_9039 computes vertical-edge Y first, then horizontal-edge X using
     * the same -$10 sequence. Only hook the second occurrence, otherwise a
     * left widescreen setting would incorrectly shift vertical block rows. */
    for (size_t off = 0; off + 20u <= rom_size; ++off) {
        if ((off >> 14) != ((off + 19u) >> 14)) continue;
        if (!wl3_left_spawn_signature(rom + off)) continue;
        size_t second_min = off + 24u;
        size_t second_max = off + 100u;
        if (second_max > rom_size) second_max = rom_size;
        if ((off >> 14) != ((second_max - 1u) >> 14))
            second_max = ((off >> 14) + 1u) << 14;
        for (size_t q = second_min; q + 20u <= second_max; ++q) {
            if ((q >> 14) != ((q + 19u) >> 14)) break;
            if (wl3_left_spawn_signature(rom + q)) {
                if (wl3_left_spawn_count < WL3_MAX_EDGE_HOOKS)
                    wl3_left_spawn_adds[wl3_left_spawn_count++] = q + 12u;
                break;
            }
        }
    }

    for (size_t off = 0; off + 5u <= rom_size; ++off) {
        if ((off >> 14) != ((off + 4u) >> 14)) continue;
        const uint8_t *p = rom + off;
        if (p[0] == 0x1E && p[1] == 0x0D && p[2] == 0xCB &&
            p[3] == 0x7E && p[4] == 0x28 &&
            wl3_col_loop_count < WL3_MAX_LOOP_HOOKS)
            wl3_col_loop_loads[wl3_col_loop_count++] = off;
    }
}

static int wl3_pc_is_bound_left(uint16_t pc)
{
    for (size_t i = 0; i < wl3_bound_left_count; ++i)
        if (wl3_rom_offset_is_mapped(wl3_bound_left_hooks[i], pc)) return 1;
    return 0;
}

static int wl3_pc_is_bound_width(uint16_t pc)
{
    for (size_t i = 0; i < wl3_bound_width_count; ++i)
        if (wl3_rom_offset_is_mapped(wl3_bound_width_hooks[i], pc)) return 1;
    return 0;
}

static int wl3_add_hl_de(uint16_t de)
{
    unsigned hl = (unsigned)((cpu.h << 8) | cpu.l);
    unsigned sum = hl + (unsigned)de;
    cpu.f = (uint8_t)((cpu.f & 0x80u) |
        ((((hl & 0x0FFFu) + ((unsigned)de & 0x0FFFu)) > 0x0FFFu) ? 0x20u : 0u) |
        (sum > 0xFFFFu ? 0x10u : 0u));
    cpu.h = (uint8_t)(sum >> 8);
    cpu.l = (uint8_t)sum;
    cpu.pc = (uint16_t)(cpu.pc + 1u);
    hw_tick(8);
    return 1;
}

/* Called before each interpreted instruction. Returning one means that the
 * original instruction was emulated here; returning zero lets it execute
 * normally after any register adjustment. */
int wide_intercept_wl3(uint8_t op)
{
    if (!wl3_profile_active || !rom || !cart_hi) return 0;

    if (op == 0x7Bu && wl3_left_extra > 0 && wl3_pc_is_bound_left(cpu.pc)) {
        unsigned hl = (unsigned)((cpu.h << 8) | cpu.l);
        hl = (hl - (unsigned)wl3_left_extra) & 0xFFFFu;
        cpu.h = (uint8_t)(hl >> 8);
        cpu.l = (uint8_t)hl;
        return 0; /* LD A,E still runs; the widened camera bound is in HL. */
    }

    if (op == 0xC6u && wl3_right_extra + wl3_left_extra > 0 &&
        wl3_pc_is_bound_width(cpu.pc)) {
        size_t off = cpu.pc < 0x4000u ? (size_t)cpu.pc
            : (size_t)(cart_hi - rom) + (size_t)(cpu.pc - 0x4000u);
        if (off + 1u >= wl3_rom_size || rom[off] != 0xC6u || rom[off + 1u] != 0xA0u)
            return 0;
        unsigned add = 160u + (unsigned)wl3_right_extra;
        unsigned sum = (unsigned)cpu.a + add;
        cpu.f = (uint8_t)(((sum & 0xFFu) == 0 ? 0x80u : 0u) |
            ((((cpu.a & 0x0Fu) + (add & 0x0Fu)) > 0x0Fu) ? 0x20u : 0u) |
            (sum > 0xFFu ? 0x10u : 0u));
        cpu.a = (uint8_t)sum;
        cpu.pc = (uint16_t)(cpu.pc + 2u);
        hw_tick(8);
        return 1;
    }

    if (op == 0x19u) { /* ADD HL,DE: widen the game's edge-spawn look-ahead. */
        for (size_t i = 0; i < wl3_right_spawn_count; ++i) {
            size_t off = wl3_right_spawn_adds[i];
            if (wl3_rom_offset_is_mapped(off, cpu.pc) && rom[off] == 0x19u)
                return wl3_add_hl_de((uint16_t)(0x00B0u + (unsigned)wl3_right_extra));
        }
        for (size_t i = 0; i < wl3_left_spawn_count; ++i) {
            size_t off = wl3_left_spawn_adds[i];
            if (wl3_rom_offset_is_mapped(off, cpu.pc) && rom[off] == 0x19u)
                return wl3_add_hl_de((uint16_t)(0u - (0x0010u + (unsigned)wl3_left_extra)));
        }
    }

    if (op == 0x1Eu) { /* LD E,$0d: extend the cross-screen spawn scan. */
        for (size_t i = 0; i < wl3_col_loop_count; ++i) {
            size_t off = wl3_col_loop_loads[i];
            if (!wl3_rom_offset_is_mapped(off, cpu.pc) ||
                rom[off] != 0x1Eu || rom[off + 1u] != 0x0Du)
                continue;
            unsigned extra_columns = ((unsigned)wl3_left_extra +
                                      (unsigned)wl3_right_extra + 15u) / 16u;
            cpu.e = (uint8_t)(0x0Du + extra_columns);
            cpu.pc = (uint16_t)(cpu.pc + 2u);
            hw_tick(8);
            return 1;
        }
    }

    return 0;
}



/* Wario Land 3 stores the level block map in SRAM banks 1-3, with 160
 * sixteen-pixel blocks per row in a 256-byte stride. The top bit is a marker
 * used by the game; the low seven bits index the active room's tile table. */
int wide_wl3_block_at(int world_x, int world_y, uint8_t *block)
{
    if (!block) return 0;
    if (!wl3_profile_active || !rom || world_x < 0 || world_y < 0)
        return 0;
#if defined(__GNUC__) || defined(__clang__)
    if (&cart_ram == NULL || cart_info == NULL) return 0;
#endif
    uint8_t *ram = cart_ram;
    const CartInfo *ci = cart_info();
    if (!ram || !ci || ci->ram_bytes < 0x8000u) return 0;

    unsigned col = (unsigned)world_x >> 4;
    unsigned row = (unsigned)world_y >> 4;
    if (col >= 160u || row >= 96u) return -1;

    size_t offset = 0x2000u + (size_t)(row / 32u) * 0x2000u +
                    (size_t)(row % 32u) * 0x100u + col;
    if (offset >= ci->ram_bytes) return 0;
    *block = (uint8_t)(ram[offset] & 0x7Fu);
    return 1;
}

int wide_wl3_block_tile(int block, int part, uint8_t *tile)
{
    if (!tile || !wl3_profile_active || !wl3_room_tiles_addr ||
        block < 0 || block >= 128 || part < 0 || part >= 4)
        return 0;
    uint16_t address = (uint16_t)(wl3_room_tiles_addr + block * 4 + part);
    *tile = rd8(address);
    return 1;
}

int wide_wl3_block_attr(int block, int part, uint8_t *attr)
{
    if (!attr || !wl3_profile_active || block < 0 || block >= 128 ||
        part < 0 || part >= 4) return 0;
#if defined(__GNUC__) || defined(__clang__)
    if (gb_wram_peek == NULL) return 0;
#endif
    /* GFX RAM is physical WRAM bank 3; wRoomBlockAttributes follows 16
     * eight-color palettes at D280, making its backing-store offset 0x3300. */
    size_t offset = 0x3300u + (size_t)block * 4u + (size_t)part;
    uint8_t value = gb_wram_peek(offset);
    if (value == 0xFFu) return 0;
    *attr = value;
    return 1;
}

int wide_wl3_camera(int *camera_x, int *camera_y)
{
    if (!wl3_profile_active || !wl3_camera_x_addr ||
        !camera_x || !camera_y) return 0;
    uint16_t xaddr = wl3_camera_x_addr;
    uint16_t yaddr = (uint16_t)(xaddr - 2u);
    *camera_x = ((int)rd8(xaddr) << 8) | rd8((uint16_t)(xaddr + 1u));
    *camera_y = ((int)rd8(yaddr) << 8) | rd8((uint16_t)(yaddr + 1u));
    return 1;
}

static int wide_install_wario_sml3(int wario_dx)
{
    static const uint8_t original_fov[] = {0xC6, 0x10, 0xFE, 0xD0};
    static const uint8_t clean_fov[]    = {0xC6, 0x21, 0xFE, 0xE2};
    static const uint8_t dx_fov[]       = {0xC6, 0x1E, 0xFE, 0xED};
    if (!rom) return 0;

    /* The uploaded DX IPS expands the clean 512 KiB ROM to 1 MiB and changes
     * the header to CGB-only MBC5 (type 1B, ROM-size code 05). */
    if (wario_dx) {
        if (rom[0x143] != 0xC0 || rom[0x147] != 0x1B || rom[0x148] != 0x05)
            return 0;
    } else if (rom[0x148] != 0x04) {
        return 0;
    }

    const uint8_t *target_fov = wario_dx ? dx_fov : clean_fov;
    int already_patched = expect(0x3218, target_fov, (int)sizeof dx_fov);
    if (!already_patched &&
        !expect(0x3218, original_fov, (int)sizeof original_fov))
        return 0;

    if (!already_patched) {
        rom[0x3219] = wario_dx ? 0x1E : 0x21;
        rom[0x321B] = wario_dx ? 0xED : 0xE2;
    }

    /* Keep the game's actor lifecycle and screen-boundary return intact.
     * The PPU extends the picture, but broadening all actor activation checks
     * was causing gameplay regressions in this slot-limited actor engine.
     * The DX-only one-byte adjustment below is kept because its instruction
     * signature is known and independently validated. */
    if (wario_dx) wario_patch_dx_activation_operand();
    return 1;
}
int wide_install_profile(int game, int l, int r, int wario_dx)
{
    /* Flush a previous game's capture before replacing the active profile. */
    wide_wl2_trace_finish();
    wl2_profile_active = 0;
    wl2_profile_game = -1;
    sml2_right_extra = 0;
    sml2_left_extra = 0;
    sml2_compositor_installed = 0;
    sml2_compositor_frame_ok = 0;
    sml2_compositor_homeworld = 0;
    sml2_compositor_left = sml2_compositor_right = 0;
    sml2_actor_build_count = sml2_actor_frame_count = 0;
    sml2_scan_edge[0] = sml2_scan_edge[1] = -1;
    sml2_scan_pick[0] = sml2_scan_pick[1] = 0;
    sml2_scan_frame[0] = sml2_scan_frame[1] = -1;
    wl3_profile_active = 0;
    wl3_left_extra = wl3_right_extra = 0;
    wl3_rom_size = 0;
    wl3_camera_x_addr = wl3_room_tiles_addr = 0;
    wl3_bound_left_count = wl3_bound_width_count = wl3_right_spawn_count = wl3_left_spawn_count = wl3_col_loop_count = 0;
    if (l + r == 0) return 1;
    if (game == GAME_WARIO_LAND2_GB || game == GAME_WARIO_LAND2_GBC) {
        /* Distinguish the monochrome and Color cartridges before widening. */
        if (!rom) return 0;
        int color_cartridge = (rom[0x143] & 0x80) != 0;
        if (color_cartridge != (game == GAME_WARIO_LAND2_GBC)) return 0;
        wl2_profile_game = game;
        wl2_profile_active = 1;
        if (l + r > 0) wl2_trace_begin(game, l, r);
        return 1;
    }
    if (game == GAME_WARIO_LAND3_GBC) {
        /* Wario Land 3 is a CGB cartridge. Discover its camera/object
         * windows from instruction signatures in the loaded image. The hooks
         * alter only live CPU registers and scan bounds; the ROM stays intact.
         * If a revision differs, retain the generic PPU widening and leave the
         * unmatched game logic untouched. */
        const CartInfo *ci = cart_info ? cart_info() : NULL;
        if (!rom || !ci || ci->size < 0x80000u || (rom[0x143] & 0x80u) == 0)
            return 0;
        wl3_left_extra = l < 0 ? 0 : (l > 32 ? 32 : l);
        wl3_right_extra = r < 0 ? 0 : (r > 32 ? 32 : r);
        wl3_profile_active = wl3_left_extra + wl3_right_extra > 0;
        wl3_scan_runtime_hooks(ci->size);
        return 1;
    }
    if (game == GAME_WARIO_SML3) return wide_install_wario_sml3(wario_dx);
    if (game == GAME_SML2) {
        /* SML2's entity activation code lives in bank 2.  Its horizontal
         * look-ahead bounds are formed by a small sequence of ADD HL,DE
         * instructions.  We intercept those instructions instead of rewriting
         * a variable-length instruction stream in the ROM.  This is especially
         * appropriate here because SML2 already runs through the interpreter. */
        if (r > 0x9F || rom[0x801C] != 0x19) return 0;
        sml2_right_extra = r;
        sml2_left_extra = l;
        sml2_compositor_installed = 1;
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

int wide_install(int game, int l, int r)
{
    return wide_install_profile(game, l, r, 0);
}