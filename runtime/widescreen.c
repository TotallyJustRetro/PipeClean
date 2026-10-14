#include <string.h>
#include <stdlib.h>
#include <ctype.h>
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
    return text_has_dx(title) || text_has_dx(path);
}

void wide_dims_profile(int game, int pct, int wario_dx, int *l, int *r)
{
    *l = *r = 0;
    if (pct <= 0) return;
    if (game == GAME_WARIO_SML3) {
        /* Wario Land's culling code is tuned to the original viewport. Keep the
         * clean ROM and the DX patch on their respective, verified margins. */
        int margin = (wario_dx ? 30 : 33) * pct;
        margin = (margin + 50) / 100;
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

static int wario_dx_source_sites_ok(int already_patched)
{
    /* Check the instruction at each operand before changing it. These sites
     * widen the DX build's object/camera margins without copying its renderer. */
    const uint8_t ret = already_patched ? 0x00 : 0xD0;
    const uint8_t bc_low = already_patched ? 0xD1 : 0xD0;
    const uint8_t sub_seven = already_patched ? 0x08 : 0x07;
    const uint8_t load_e = already_patched ? 0x10 : 0x0E;
    const uint8_t add_six = already_patched ? 0x08 : 0x06;

    if (rom[0x321C] != ret) return 0;
    if (rom[0x31FC] != 0x01 || rom[0x31FD] != bc_low || rom[0x31FE] != 0x00) return 0;
    if (rom[0x8476] != 0xD6 || rom[0x8477] != sub_seven) return 0;
    if (rom[0x8485] != 0x1E || rom[0x8486] != load_e) return 0;
    if (rom[0x849F] != 0xC6 || rom[0x84A0] != add_six) return 0;
    if (rom[0x84BF] != 0xD6 || rom[0x84C0] != sub_seven) return 0;
    return 1;
}

static int wide_install_wario_sml3(int wario_dx)
{
    static const uint8_t original_fov[] = {0xC6, 0x10, 0xFE, 0xD0};
    static const uint8_t clean_fov[] =    {0xC6, 0x21, 0xFE, 0xE2};
    static const uint8_t dx_fov[] =       {0xC6, 0x1E, 0xFE, 0xED};
    /* These offsets are tied to the 512 KiB World ROM family. The cartridge
     * header's ROM-size code is 4 for 512 KiB; unsupported images fall back to
     * the original-width display before any write occurs. */
    if (!rom || rom[0x148] != 0x04) return 0;

    if (!wario_dx) {
        if (expect(0x3218, clean_fov, (int)sizeof clean_fov)) return 1;
        if (!expect(0x3218, original_fov, (int)sizeof original_fov)) return 0;
        rom[0x3219] = 0x21; /* expand the left/object visibility margin */
        rom[0x321B] = 0xE2; /* permit objects beyond the original right edge */
        return 1;
    }

    if (expect(0x3218, dx_fov, (int)sizeof dx_fov) &&
        wario_dx_source_sites_ok(1)) return 1;
    if (!expect(0x3218, original_fov, (int)sizeof original_fov) ||
        !wario_dx_source_sites_ok(0)) return 0;

    /* All sites were validated above, so the loaded cartridge is either
     * changed completely or left untouched on a mismatch. The source ROM file
     * on disk is never written. */
    rom[0x3219] = 0x1E;
    rom[0x321B] = 0xED;
    rom[0x321C] = 0x00;
    rom[0x31FD] = 0xD1;
    rom[0x8477] = 0x08;
    rom[0x8486] = 0x10;
    rom[0x84A0] = 0x08;
    rom[0x84C0] = 0x08;
    return 1;
}

int wide_install_profile(int game, int l, int r, int wario_dx)
{
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
    if (l + r == 0) return 1;
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