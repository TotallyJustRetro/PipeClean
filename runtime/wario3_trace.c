/* Runtime-only diagnostics for Wario Land 3 widescreen investigation.
 * This module observes the running game and never patches the ROM. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "gb.h"
#include "cart.h"
#include "widescreen.h"

#define WL3_PC_SLOTS 8192u
#define WL3_OAM_EVENTS 8192u
#define WL3_MEM_SLOTS 0xA000u

int wide_wl3_trace_active;

typedef struct {
    uint32_t offset, hits, first_frame, last_frame;
    uint16_t pc;
    uint8_t opcode, bytes[4];
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t sp;
    uint8_t scx, scy, lcdc, vbk;
    int used;
} Wl3PcEntry;

typedef struct {
    uint32_t hits, first_frame, last_frame;
    uint16_t address, last_pc;
    uint8_t first_value, min_value, max_value, old_value, new_value;
    uint8_t opcode, bank, used;
} Wl3MemEntry;

typedef struct {
    uint32_t frame;
    uint8_t slot, from_region, to_region;
    uint8_t old_y, old_x, old_tile, old_attr;
    uint8_t y, x, tile, attr, scx, scy;
    int16_t old_sx, old_sy, sx, sy;
} Wl3OamEvent;

static Wl3PcEntry pc_entries[WL3_PC_SLOTS];
static size_t pc_count;
static Wl3MemEntry mem_entries[WL3_MEM_SLOTS];
static Wl3OamEvent oam_events[WL3_OAM_EVENTS];
static size_t oam_event_count;
static uint32_t oam_events_dropped;
static uint8_t oam_previous[0xA0];
static int oam_previous_valid;
static int wl3_left, wl3_right, wl3_trace_started, wl3_atexit_registered;
static size_t wl3_rom_size;
static uint32_t wl3_crc;
static char wl3_title[17];
static char pc_path[1200] = "pipeclean-wl3-gbc-wide-trace.txt";
static char mem_path[1200] = "pipeclean-wl3-gbc-memory-writes.txt";
static char oam_path[1200] = "pipeclean-wl3-gbc-oam-events.txt";

static int wl3_rom_offset(uint16_t pc, size_t *offset)
{
    if (!rom || !offset || pc >= 0x8000u) return 0;
    const uint8_t *mapped = pc < 0x4000u ? cart_lo : cart_hi;
    if (!mapped || (uintptr_t)mapped < (uintptr_t)rom) return 0;
    size_t base = (size_t)((uintptr_t)mapped - (uintptr_t)rom);
    size_t physical = base + (size_t)(pc & 0x3FFFu);
    if (physical >= wl3_rom_size) return 0;
    *offset = physical;
    return 1;
}

static size_t wl3_pc_slot(size_t offset, uint8_t opcode)
{
    uint32_t hash = (uint32_t)(offset ^ (offset >> 13) ^ ((uint32_t)opcode * 2654435761u));
    size_t start = (size_t)hash & (WL3_PC_SLOTS - 1u);
    for (size_t probe = 0; probe < WL3_PC_SLOTS; ++probe) {
        size_t slot = (start + probe) & (WL3_PC_SLOTS - 1u);
        Wl3PcEntry *e = &pc_entries[slot];
        if (!e->used) {
            if (pc_count >= WL3_PC_SLOTS / 2u) return WL3_PC_SLOTS;
            memset(e, 0, sizeof *e);
            e->used = 1;
            e->offset = (uint32_t)offset;
            e->opcode = opcode;
            pc_count++;
            return slot;
        }
        if (e->offset == offset && e->opcode == opcode) return slot;
    }
    return WL3_PC_SLOTS;
}

void wide_wl3_trace_instruction(uint16_t pc)
{
    if (!wide_wl3_trace_active || !rom || !cart_lo || !cart_hi) return;
    size_t offset;
    if (!wl3_rom_offset(pc, &offset)) return;
    uint8_t opcode = rd8(pc);
    size_t slot = wl3_pc_slot(offset, opcode);
    if (slot >= WL3_PC_SLOTS) return;
    Wl3PcEntry *e = &pc_entries[slot];
    uint32_t frame = frame_count < 0 ? 0u : (uint32_t)frame_count;
    if (!e->hits) {
        e->first_frame = frame;
        for (unsigned i = 0; i < sizeof e->bytes; ++i)
            e->bytes[i] = offset + i < wl3_rom_size ? rom[offset + i] : 0xFFu;
    }
    e->hits++;
    e->last_frame = frame;
    e->pc = pc;
    e->a = cpu.a; e->f = cpu.f;
    e->b = cpu.b; e->c = cpu.c;
    e->d = cpu.d; e->e = cpu.e;
    e->h = cpu.h; e->l = cpu.l;
    e->sp = cpu.sp;
    e->scx = rd8(0xFF43u);
    e->scy = rd8(0xFF42u);
    e->lcdc = rd8(0xFF40u);
    e->vbk = rd8(0xFF4Fu);
}

void wide_wl3_trace_memory_write(uint16_t address, uint8_t old_value, uint8_t new_value)
{
    if (!wide_wl3_trace_active) return;
    size_t index;
    uint8_t bank = 0xFFu; /* cartridge RAM bank is mapper-controlled */
    if (address >= 0xA000u && address < 0xC000u) {
        index = (size_t)(address - 0xA000u);
    } else if (address >= 0xC000u && address < 0xD000u) {
        index = 0x2000u + (size_t)(address - 0xC000u);
        bank = 0;
    } else if (address >= 0xD000u && address < 0xE000u) {
        bank = (uint8_t)(rd8(0xFF70u) & 7u);
        if (!bank) bank = 1;
        index = 0x3000u + (size_t)(bank - 1u) * 0x1000u + (size_t)(address - 0xD000u);
    } else {
        return;
    }
    if (index >= WL3_MEM_SLOTS) return;
    Wl3MemEntry *e = &mem_entries[index];
    uint32_t frame = frame_count < 0 ? 0u : (uint32_t)frame_count;
    if (!e->used) {
        e->used = 1;
        e->address = address;
        e->bank = bank;
        e->first_frame = frame;
        e->first_value = new_value;
        e->min_value = new_value;
        e->max_value = new_value;
    }
    e->hits++;
    e->last_frame = frame;
    e->last_pc = cpu.pc;
    e->old_value = old_value;
    e->new_value = new_value;
    if (new_value < e->min_value) e->min_value = new_value;
    if (new_value > e->max_value) e->max_value = new_value;
    size_t offset;
    e->opcode = wl3_rom_offset(cpu.pc, &offset) ? rom[offset] : 0xFFu;
}

static uint8_t wl3_oam_region(uint8_t raw_y, uint8_t raw_x,
                              int *screen_x, int *screen_y)
{
    int x = (int)raw_x;
    int neg = wl3_left > 8 ? 256 - (wl3_left - 8) : 256;
    if (x >= neg) x -= 256;
    x = x - 8 + wl3_left;
    int y = (int)raw_y - 16;
    if (screen_x) *screen_x = x;
    if (screen_y) *screen_y = y;
    int total_w = 160 + wl3_left + wl3_right;
    if (raw_y == 0 || raw_y >= 160 || x + 8 <= 0 || x >= total_w) return 0;
    if (x < wl3_left) return 1;
    if (x + 8 > wl3_left + 160) return 3;
    return 2;
}

void wide_wl3_trace_frame(const uint8_t *snapshot)
{
    if (!wide_wl3_trace_active || !snapshot) return;
    uint32_t frame = frame_count < 0 ? 0u : (uint32_t)frame_count;
    uint8_t scx = rd8(0xFF43u), scy = rd8(0xFF42u);
    for (unsigned i = 0; i < 40; ++i) {
        size_t b = i * 4u;
        uint8_t y = snapshot[b], x = snapshot[b+1u];
        uint8_t tile = snapshot[b+2u], attr = snapshot[b+3u];
        uint8_t oy = oam_previous[b], ox = oam_previous[b+1u];
        uint8_t ot = oam_previous[b+2u], oa = oam_previous[b+3u];
        int sx = 0, sy = 0, osx = 0, osy = 0;
        uint8_t to = wl3_oam_region(y, x, &sx, &sy);
        uint8_t from = oam_previous_valid ? wl3_oam_region(oy, ox, &osx, &osy) : 0;
        int record = !oam_previous_valid ? (to != 0) : (from != to);
        if (!record && to != 0 && (abs(sx-osx) > 8 || abs(sy-osy) > 8)) record = 1;
        if (!record || oam_event_count >= WL3_OAM_EVENTS) {
            if (record) oam_events_dropped++;
            continue;
        }
        Wl3OamEvent *e = &oam_events[oam_event_count++];
        e->frame = frame; e->slot = (uint8_t)i;
        e->from_region = from; e->to_region = to;
        e->old_y = oy; e->old_x = ox; e->old_tile = ot; e->old_attr = oa;
        e->y = y; e->x = x; e->tile = tile; e->attr = attr;
        e->scx = scx; e->scy = scy;
        e->old_sx = (int16_t)osx; e->old_sy = (int16_t)osy;
        e->sx = (int16_t)sx; e->sy = (int16_t)sy;
    }
    memcpy(oam_previous, snapshot, sizeof oam_previous);
    oam_previous_valid = 1;
}

static int wl3_pc_order(const void *a, const void *b)
{
    const Wl3PcEntry *const *x = (const Wl3PcEntry *const *)a;
    const Wl3PcEntry *const *y = (const Wl3PcEntry *const *)b;
    if ((*x)->hits < (*y)->hits) return 1;
    if ((*x)->hits > (*y)->hits) return -1;
    return (*x)->offset < (*y)->offset ? -1 : ((*x)->offset > (*y)->offset);
}

static const char *wl3_region_name(uint8_t region)
{
    return region == 1 ? "LEFT" : region == 2 ? "NATIVE" : region == 3 ? "RIGHT" : "OFF";
}

void wide_wl3_trace_set_path(const char *path)
{
    if (!path || !path[0]) return;
    const char *forward = strrchr(path, '/');
    const char *backward = strrchr(path, '\\');
    const char *separator = forward;
    if (!separator || (backward && backward > separator)) separator = backward;
    size_t n = separator ? (size_t)(separator - path + 1) : 0;
    char dir[1200];
    if (n >= sizeof dir) n = sizeof dir - 1;
    memcpy(dir, path, n); dir[n] = '\0';
    snprintf(pc_path, sizeof pc_path, "%spipeclean-wl3-gbc-wide-trace.txt", dir);
    snprintf(mem_path, sizeof mem_path, "%spipeclean-wl3-gbc-memory-writes.txt", dir);
    snprintf(oam_path, sizeof oam_path, "%spipeclean-wl3-gbc-oam-events.txt", dir);
}

void wide_wl3_trace_finish(void)
{
    wide_wl3_trace_active = 0;
    if (!wl3_trace_started) return;
    wl3_trace_started = 0;

    Wl3PcEntry **sorted = pc_count ? (Wl3PcEntry **)malloc(pc_count * sizeof *sorted) : NULL;
    if (pc_count && sorted) {
        size_t n = 0;
        for (size_t i = 0; i < WL3_PC_SLOTS; ++i)
            if (pc_entries[i].used) sorted[n++] = &pc_entries[i];
        qsort(sorted, n, sizeof *sorted, wl3_pc_order);
    }
    FILE *f = fopen(pc_path, "w");
    if (f) {
        fprintf(f, "PipeClean Wario Land 3 widescreen runtime trace v1\n");
        fprintf(f, "Game: Wario Land 3 (GBC); CRC32=%08X ROM bytes=%zu title=%s\n",
                (unsigned)wl3_crc, wl3_rom_size, wl3_title);
        fprintf(f, "Margins: left=%d right=%d; frames=%d; unique PCs=%zu\n",
                wl3_left, wl3_right, frame_count, pc_count);
        fprintf(f, "physicalROMOffset pc hits firstFrame lastFrame opcode bytes A F B C D E H L SP SCX SCY LCDC VBK\n");
        if (sorted) for (size_t i = 0; i < pc_count; ++i) {
            const Wl3PcEntry *e = sorted[i];
            fprintf(f, "%06X %04X %u %u %u %02X %02X%02X%02X%02X %02X %02X %02X %02X %02X %02X %02X %02X %04X %02X %02X %02X %02X\n",
                    (unsigned)e->offset, e->pc, (unsigned)e->hits,
                    (unsigned)e->first_frame, (unsigned)e->last_frame,
                    e->opcode, e->bytes[0], e->bytes[1], e->bytes[2], e->bytes[3],
                    e->a, e->f, e->b, e->c, e->d, e->e, e->h, e->l,
                    e->sp, e->scx, e->scy, e->lcdc, e->vbk);
        }
        fclose(f);
    } else {
        fprintf(stderr, "PipeClean: couldn't write Wario Land 3 trace: %s\n", pc_path);
    }
    free(sorted);

    f = fopen(mem_path, "w");
    if (f) {
        fprintf(f, "PipeClean Wario Land 3 WRAM/ cartridge RAM writes v1\n");
        fprintf(f, "Address bank writes firstFrame lastFrame firstValue minValue maxValue lastOld lastNew lastPC opcode\n");
        for (size_t i = 0; i < WL3_MEM_SLOTS; ++i) {
            const Wl3MemEntry *e = &mem_entries[i];
            if (!e->used) continue;
            fprintf(f, "%04X %02X %u %u %u %02X %02X %02X %02X %02X %04X %02X\n",
                    e->address, e->bank, (unsigned)e->hits, (unsigned)e->first_frame,
                    (unsigned)e->last_frame, e->first_value, e->min_value, e->max_value,
                    e->old_value, e->new_value, e->last_pc, e->opcode);
        }
        fclose(f);
    } else {
        fprintf(stderr, "PipeClean: couldn't write Wario Land 3 memory trace: %s\n", mem_path);
    }

    f = fopen(oam_path, "w");
    if (f) {
        fprintf(f, "PipeClean Wario Land 3 OAM visibility events v1\n");
        fprintf(f, "Margins: left=%d right=%d events=%zu dropped=%u\n",
                wl3_left, wl3_right, oam_event_count, (unsigned)oam_events_dropped);
        fprintf(f, "frame slot from>to oldY oldX oldTile oldAttr newY newX newTile newAttr screenX screenY oldScreenX oldScreenY SCX SCY\n");
        for (size_t i = 0; i < oam_event_count; ++i) {
            const Wl3OamEvent *e = &oam_events[i];
            fprintf(f, "%u %u %s>%s %02X %02X %02X %02X %02X %02X %02X %02X %d %d %d %d %02X %02X\n",
                    (unsigned)e->frame, (unsigned)e->slot,
                    wl3_region_name(e->from_region), wl3_region_name(e->to_region),
                    e->old_y, e->old_x, e->old_tile, e->old_attr,
                    e->y, e->x, e->tile, e->attr, (int)e->sx, (int)e->sy,
                    (int)e->old_sx, (int)e->old_sy, e->scx, e->scy);
        }
        fclose(f);
    } else {
        fprintf(stderr, "PipeClean: couldn't write Wario Land 3 OAM trace: %s\n", oam_path);
    }
    fprintf(stderr, "PipeClean: wrote Wario Land 3 diagnostics: %s, %s, %s\n",
            pc_path, mem_path, oam_path);
}

void wide_wl3_trace_begin(int left, int right)
{
    wide_wl3_trace_finish();
    if (left + right <= 0) return;
    memset(pc_entries, 0, sizeof pc_entries);
    memset(mem_entries, 0, sizeof mem_entries);
    memset(oam_events, 0, sizeof oam_events);
    memset(oam_previous, 0, sizeof oam_previous);
    pc_count = oam_event_count = 0;
    oam_events_dropped = 0;
    oam_previous_valid = 0;
    wl3_left = left; wl3_right = right;
    const CartInfo *ci = cart_info();
    wl3_rom_size = ci ? ci->size : 0;
    wl3_crc = ci ? ci->crc : 0;
    memset(wl3_title, 0, sizeof wl3_title);
    if (ci) { memcpy(wl3_title, ci->title, sizeof wl3_title - 1u); wl3_title[sizeof wl3_title - 1u] = '\0'; }
    else memcpy(wl3_title, "unknown", sizeof "unknown");
    wl3_trace_started = 1;
    wide_wl3_trace_active = 1;
    if (!wl3_atexit_registered) {
        atexit(wide_wl3_trace_finish);
        wl3_atexit_registered = 1;
    }
}
