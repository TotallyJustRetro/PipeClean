/* DMG PPU: scanline renderer with mode timing and STAT interrupts.
 * Each line is drawn when mode 3 ends, using the registers as they are then,
 * so per-line raster effects (SCX/SCY/palette changes in an HBlank/LYC
 * handler) work. */
#include "gb.h"
#include "widescreen.h"

/* ppu.c is also compiled alone by the CGB renderer unit test. Keep the
 * compositor an optional link-time feature for that target. */
#if defined(__GNUC__) || defined(__clang__)
extern void wide_sml2_prepare_frame(int, int, int, int, int, int, int,
                                    const uint8_t *) __attribute__((weak));
extern int wide_sml2_margin_tile(int, int, uint8_t *, int *, int *) __attribute__((weak));
extern void wide_sml2_snapshot_actors(void) __attribute__((weak));
extern const WideSml2Sprite *wide_sml2_actor_sprites(int *) __attribute__((weak));
extern int wide_sml2_world_origin(int *, int *) __attribute__((weak));
extern int wide_sml2_homeworld_active(int, int, int) __attribute__((weak));
extern int wide_sml2_homeworld_tile_at(int, int, uint8_t *) __attribute__((weak));
extern int wide_sml2_homeworld_camera(int, int, int *, int *) __attribute__((weak));
#endif

uint8_t vram[0x2000], oam[0xA0];
static uint8_t vram_cgb1[0x2000];
static uint8_t vram_bank;
static uint8_t cgb_bg_palette[64], cgb_obj_palette[64];
static uint8_t cgb_bgpi, cgb_obpi, cgb_opri;
uint8_t ppu_shade[GB_H][GB_WMAX];
uint32_t ppu_rgb[GB_H][GB_WMAX]; /* Native 24-bit RGB for CGB-mode pixels. */
static int cgb_render_enabled;
uint8_t ppu_layer[GB_H][GB_WMAX];
uint16_t ppu_bgtile[GB_H][GB_WMAX], ppu_sprtile[GB_H][GB_WMAX];   /* tile number 0..383, 0xFFFF = none */
uint8_t ppu_bguv[GB_H][GB_WMAX], ppu_spruv[GB_H][GB_WMAX];
int ppu_w = GB_W, ppu_xoff = 0;
static int hud_lines, hud_shift, wide_r, sprite_neg = 256, win_centre, wide_gate, wide_on = 1, cfg_hud, cfg_win;
static int cfg_left, cfg_right;
/* True only while the SML2 Homeworld's whole raw map is rendered as an overview. */
static int ppu_homeworld_overview;
void ppu_set_wide(int l, int r, int hud, int centre_win, int gate)
{
    if (l < 0) l = 0;
    if (r < 0) r = 0;
    if (l + r > GB_WMAX - GB_W) r = GB_WMAX - GB_W - l;
    cfg_left = l; cfg_right = r;
    ppu_xoff = l; wide_r = r; ppu_w = GB_W + l + r;
    hud_lines = (l + r) ? hud : 0; hud_shift = (r - l) / 2;
    win_centre = (l + r) ? centre_win : 0;
    cfg_hud = hud_lines; cfg_win = win_centre; wide_gate = gate; wide_on = 1;
    sprite_neg = l > 8 ? 256 - (l - 8) : 256;
}       /* (v << 3) | u inside the tile as stored (before flips) */

static uint8_t lcdc, stat_sel, scy, scx, lyc, bgp, obp0, obp1, wy, wx;
static int dot, ly, mode, lcd_on, stat_line, wlc;
static int off_cycles;
int frame_count;

/* The Homeworld's 512px source map is shown at its full native map width.
 * Move the original 160px viewport inside the 512px canvas based on its live
 * camera position, so the added view reveals both map edges without blank space. */
static void ppu_refresh_wide_layout(void)
{
    int l = cfg_left, r = cfg_right;
    ppu_homeworld_overview = 0;
    int homeworld = wide_on && cfg_left + cfg_right > 0 &&
                    wide_sml2_homeworld_active &&
                    wide_sml2_homeworld_active(lcdc, wx, wy) &&
                    wide_sml2_homeworld_tile_at &&
                    wide_sml2_homeworld_camera;
    if (homeworld) {
        /* Preserve the normal widescreen output size. The Homeworld itself is
           redrawn from its complete 512x256 source map into this frame. */
        l = 0;
        r = cfg_left + cfg_right;
        ppu_homeworld_overview = 1;
    }
    if (l < 0) l = 0;
    if (r < 0) r = 0;
    if (l + r > GB_WMAX - GB_W) r = GB_WMAX - GB_W - l;
    ppu_xoff = ppu_homeworld_overview ? 0 : l;
    wide_r = ppu_homeworld_overview ? 0 : r;
    ppu_w = GB_W + l + r;
    hud_lines = ppu_homeworld_overview ? 0 : ((l + r) ? cfg_hud : 0);
    hud_shift = (r - l) / 2;
    win_centre = ppu_homeworld_overview ? 0 : ((l + r) ? cfg_win : 0);
    sprite_neg = l > 8 ? 256 - (l - 8) : 256;
}

void ppu_reset(void)
{
    memset(vram, 0, sizeof vram);
    memset(vram_cgb1, 0, sizeof vram_cgb1);
    vram_bank = 0;
    memset(cgb_bg_palette, 0xFF, sizeof cgb_bg_palette);
    memset(cgb_obj_palette, 0xFF, sizeof cgb_obj_palette);
    cgb_bgpi = cgb_obpi = cgb_opri = 0;
    memset(oam, 0, sizeof oam);
    memset(ppu_shade, 0, sizeof ppu_shade);
    memset(ppu_rgb, 0, sizeof ppu_rgb);
    cgb_render_enabled = 0;
    memset(ppu_layer, 0, sizeof ppu_layer);
    memset(ppu_bgtile, 0xFF, sizeof ppu_bgtile); memset(ppu_sprtile, 0xFF, sizeof ppu_sprtile);
    lcdc = 0x91; stat_sel = 0; scy = scx = lyc = 0;
    bgp = 0xFC; obp0 = obp1 = 0xFF; wy = wx = 0;
    dot = 0; ly = 0; mode = 2; lcd_on = 1; stat_line = 0; wlc = 0; off_cycles = 0;
    frame_count = 0;
}

static void update_stat(void)
{
    int line = ((stat_sel & 0x08) && mode == 0) || ((stat_sel & 0x10) && mode == 1) ||
               ((stat_sel & 0x20) && mode == 2) || ((stat_sel & 0x40) && ly == lyc);
    if (line && !stat_line) io_if |= 0x02;
    stat_line = line;
}

uint8_t ppu_vram_read(uint16_t a)
{
    a &= 0x1FFF;
    return (vram_bank & 1) ? vram_cgb1[a] : vram[a];
}

void ppu_vram_write(uint16_t a, uint8_t value)
{
    a &= 0x1FFF;
    if (vram_bank & 1) vram_cgb1[a] = value;
    else vram[a] = value;
}

uint8_t ppu_vram_bank_read(void) { return (uint8_t)(0xFE | (vram_bank & 1)); }

/* Copy the tile-data area of CGB VRAM bank 1 for frame-local overlays. */
void ppu_vram_bank1_copy(uint8_t out[0x1800])
{
    if (out) memcpy(out, vram_cgb1, 0x1800);
}

void ppu_set_cgb_mode(int enabled) { cgb_render_enabled = enabled != 0; }
int ppu_cgb_mode_enabled(void) { return cgb_render_enabled; }

static uint32_t cgb_rgb(const uint8_t *palette, unsigned pal, unsigned ci)
{
    unsigned off = (pal & 7u) * 8u + (ci & 3u) * 2u;
    unsigned c = (unsigned)palette[off] | ((unsigned)(palette[off + 1] & 0x7Fu) << 8);
    unsigned r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
    r = (r * 255u + 15u) / 31u;
    g = (g * 255u + 15u) / 31u;
    b = (b * 255u + 15u) / 31u;
    return (r << 16) | (g << 8) | b;
}

/* Snapshot the active CGB OBJ palette as RGB24 for separately rendered P2 sprites. */
void ppu_cgb_obj_palette_copy(uint32_t out[32])
{
    if (!out) return;
    for (unsigned pal = 0; pal < 8; pal++)
        for (unsigned ci = 0; ci < 4; ci++)
            out[pal * 4 + ci] = cgb_rgb(cgb_obj_palette, pal, ci);
}

/* Resolve a pixel from a tile index and attribute. Coordinates are pixel
 * coordinates in the source tile/world, not the 256x256 VRAM map ring. */
static int tile_pixel_data(uint8_t tile, uint8_t attr, int map_x, int map_y,
                           int *attr_out, int *tile_out)
{
    int row = map_y & 7, col = map_x & 7;
    if (attr & 0x40) row = 7 - row;
    if (attr & 0x20) col = 7 - col;
    int addr = (lcdc & 0x10) ? (int)tile * 16 : 0x1000 + (int)(int8_t)tile * 16;
    addr += row * 2;
    const uint8_t *tiles = ((attr & 0x08) && cgb_render_enabled) ? vram_cgb1 : vram;
    int bit = 7 - col;
    int ci = (((tiles[addr + 1] >> bit) & 1) << 1) | ((tiles[addr] >> bit) & 1);
    if (attr_out) *attr_out = attr;
    if (tile_out) *tile_out = addr >> 4;
    return ci;
}

/* Resolve one BG/window pixel from the native 256x256 tilemap ring. */
static int tile_pixel(int map_off, int map_x, int map_y, int *attr_out, int *tile_out)
{
    unsigned map_index = (unsigned)map_off + (unsigned)((map_y >> 3) & 31) * 32u +
                         (unsigned)((map_x >> 3) & 31);
    uint8_t tile = vram[map_index];
    uint8_t attr = cgb_render_enabled ? vram_cgb1[map_index] : 0;
    return tile_pixel_data(tile, attr, map_x, map_y, attr_out, tile_out);
}
void ppu_vram_bank_write(uint8_t value) { vram_bank = value & 1; }

uint8_t ppu_cgb_read(uint8_t r)
{
    switch (r) {
    case 0x68: return (uint8_t)(cgb_bgpi | 0x40);
    case 0x69: return cgb_bg_palette[cgb_bgpi & 0x3F];
    case 0x6A: return (uint8_t)(cgb_obpi | 0x40);
    case 0x6B: return cgb_obj_palette[cgb_obpi & 0x3F];
    case 0x6C: return (uint8_t)(0xFE | (cgb_opri & 1));
    default: return 0xFF;
    }
}

void ppu_cgb_write(uint8_t r, uint8_t value)
{
    switch (r) {
    case 0x68: cgb_bgpi = value & 0xBF; break;
    case 0x69:
        cgb_bg_palette[cgb_bgpi & 0x3F] = value;
        if (cgb_bgpi & 0x80) cgb_bgpi = (uint8_t)(0x80 | ((cgb_bgpi + 1) & 0x3F));
        break;
    case 0x6A: cgb_obpi = value & 0xBF; break;
    case 0x6B:
        cgb_obj_palette[cgb_obpi & 0x3F] = value;
        if (cgb_obpi & 0x80) cgb_obpi = (uint8_t)(0x80 | ((cgb_obpi + 1) & 0x3F));
        break;
    case 0x6C: cgb_opri = value & 1; break;
    }
}

uint8_t ppu_read(uint8_t r)
{
    switch (r) {
    case 0x40: return lcdc;
    case 0x41: return 0x80 | stat_sel | (lcd_on && ly == lyc ? 4 : 0) | (lcd_on ? mode : 0);
    case 0x42: return scy;
    case 0x43: return scx;
    case 0x44: return (uint8_t)ly;
    case 0x45: return lyc;
    case 0x46: return 0xFF;
    case 0x47: return bgp;
    case 0x48: return obp0;
    case 0x49: return obp1;
    case 0x4A: return wy;
    case 0x4B: return wx;
    }
    return 0xFF;
}

static void blank_screen(void)
{
    memset(ppu_shade, 0, sizeof ppu_shade); memset(ppu_rgb, 0, sizeof ppu_rgb); memset(ppu_layer, 0, sizeof ppu_layer);
    memset(ppu_bgtile, 0xFF, sizeof ppu_bgtile); memset(ppu_sprtile, 0xFF, sizeof ppu_sprtile);
}

/* FNV-1a over a tile's 16 bytes: the id used by texture packs */
static uint64_t tile_hash(const uint8_t *t)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 16; i++) { h ^= t[i]; h *= 1099511628211ull; }
    return h;
}

void ppu_tile_hashes(uint64_t out[384])
{
    for (int i = 0; i < 384; i++) out[i] = tile_hash(&vram[i * 16]);
}

void ppu_write(uint8_t r, uint8_t v)
{
    switch (r) {
    case 0x40: {
        int was = lcd_on;
        lcdc = v;
        lcd_on = (v & 0x80) != 0;
        if (was && !lcd_on) {
            ly = 0; dot = 0; mode = 0; stat_line = 0; wlc = 0; off_cycles = 0;
            blank_screen();
        } else if (!was && lcd_on) {
            ly = 0; dot = 0; mode = 2; wlc = 0;
            update_stat();
        }
        break;
    }
    case 0x41: stat_sel = v & 0x78; if (lcd_on) update_stat(); break;
    case 0x42: scy = v; break;
    case 0x43: scx = v; break;
    case 0x44: break;
    case 0x45: lyc = v; if (lcd_on) update_stat(); break;
    case 0x47: bgp = v; break;
    case 0x48: obp0 = v; break;
    case 0x49: obp1 = v; break;
    case 0x4A: wy = v; break;
    case 0x4B: wx = v; break;
    }
}

/* ------------------------------------------------------------------ */
#define HOMEWORLD_SOURCE_WIDTH 512
#define HOMEWORLD_SOURCE_HEIGHT 256

static void render_homeworld_overview_line(int y)
{
    const int W = ppu_w;
    uint8_t *out = ppu_shade[y];
    uint32_t *rgb = ppu_rgb[y];
    uint8_t bgci[GB_WMAX];
    memset(bgci, 0, sizeof bgci);
    memset(ppu_layer[y], 0, W);
    memset(ppu_bgtile[y], 0xFF, W * sizeof ppu_bgtile[y][0]);
    memset(ppu_sprtile[y], 0xFF, W * sizeof ppu_sprtile[y][0]);
    memset(ppu_bguv[y], 0, W);
    memset(ppu_spruv[y], 0, W);

    /* A 512x256 map scaled by W/512 has a height of W/2. This gives the
       complete world map a stable 2:1 aspect ratio with narrow top/bottom
       bars inside the ordinary 160px-high Game Boy frame geometry. */
    int map_h = (HOMEWORLD_SOURCE_HEIGHT * W + HOMEWORLD_SOURCE_WIDTH / 2) /
                HOMEWORLD_SOURCE_WIDTH;
    int map_area_h = (wy > 0 && wy < GB_H) ? wy : GB_H;
    int map_top = (map_area_h - map_h) / 2;
    int camera_x = 0, camera_y = 0;
    int have_camera = wide_sml2_homeworld_camera &&
        wide_sml2_homeworld_camera(scx, scy, &camera_x, &camera_y);

    for (int x = 0; x < W; x++) {
        int world_x = (x * HOMEWORLD_SOURCE_WIDTH) / W;
        if (y < map_top || y >= map_top + map_h) {
            bgci[x] = 0;
            out[x] = cgb_render_enabled ? 0 : (uint8_t)(bgp & 3);
            rgb[x] = cgb_render_enabled ? cgb_rgb(cgb_bg_palette, 0, 0) : 0;
            continue;
        }
        int world_y = ((y - map_top) * HOMEWORLD_SOURCE_WIDTH) / W;
        uint8_t tile = 0;
        int attr = 0, tile_id = 0;
        int ci = 0;
        if (wide_sml2_homeworld_tile_at &&
            wide_sml2_homeworld_tile_at(world_x, world_y, &tile)) {
            ci = tile_pixel_data(tile, 0, world_x, world_y, &attr, &tile_id);
        }
        bgci[x] = (uint8_t)ci;
        out[x] = cgb_render_enabled ? (uint8_t)ci :
                 (uint8_t)((bgp >> (ci * 2)) & 3);
        rgb[x] = cgb_render_enabled ?
                 cgb_rgb(cgb_bg_palette, (unsigned)(attr & 7), (unsigned)ci) : 0;
        ppu_bgtile[y][x] = (uint16_t)tile_id;
        ppu_bguv[y][x] = (uint8_t)(((world_y & 7) << 3) | (world_x & 7));
    }

    /* Preserve the native Homeworld status/window strip at the bottom. It
       remains UI, not part of the scaled world map, and is centred over the
       overview instead of being drawn at the old left margin. */
    if (y >= wy && (lcdc & 0x20) &&
        (cgb_render_enabled || (lcdc & 0x01)) && wx <= 166) {
        int map_off = (lcdc & 0x40) ? 0x1C00 : 0x1800;
        int native_offset = (W - GB_W) / 2;
        int window_x0 = wx - 7 + native_offset;
        int window_x1 = native_offset + GB_W;
        int window_y = y - wy;
        for (int x = window_x0 < 0 ? 0 : window_x0;
             x < W && x < window_x1; x++) {
            int px = x - window_x0, attr = 0, tile_id = 0;
            int ci = tile_pixel(map_off, px, window_y, &attr, &tile_id);
            bgci[x] = (uint8_t)ci;
            out[x] = cgb_render_enabled ? (uint8_t)ci :
                     (uint8_t)((bgp >> (ci * 2)) & 3);
            rgb[x] = cgb_render_enabled ?
                     cgb_rgb(cgb_bg_palette, (unsigned)(attr & 7),
                             (unsigned)ci) : 0;
            ppu_bgtile[y][x] = (uint16_t)tile_id;
            ppu_bguv[y][x] = (uint8_t)(((window_y & 7) << 3) | (px & 7));
        }
    }

    if (!have_camera || !(lcdc & 0x02) || y >= wy) return;

    /* OAM coordinates are camera-relative during gameplay. Convert each live
       sprite back to map coordinates, then draw it at the same overview scale. */
    typedef struct {
        int x, y, h, screen_x, index;
        uint8_t tile, flags;
    } OverviewSprite;
    OverviewSprite sprites[40];
    int n = 0;
    int sprite_h = (lcdc & 0x04) ? 16 : 8;
    for (int i = 0; i < 40; i++) {
        int oy = oam[i * 4], ox = oam[i * 4 + 1];
        if (ox <= 0 || ox >= 168 || oy <= 16 - sprite_h || oy >= 160)
            continue;
        sprites[n++] = (OverviewSprite){
            camera_x + ox - 8, camera_y + oy - 16, sprite_h, ox, i,
            oam[i * 4 + 2], oam[i * 4 + 3]
        };
    }

    if (y < map_top || y >= map_top + map_h) return;
    int world_y = ((y - map_top) * HOMEWORLD_SOURCE_WIDTH) / W;
    for (int x = 0; x < W; x++) {
        int world_x = (x * HOMEWORLD_SOURCE_WIDTH) / W;
        int best = -1, best_priority = 0x7FFFFFFF;
        int best_ci = 0, best_addr = 0, best_row = 0, best_col = 0;
        uint8_t best_flags = 0;

        for (int si = 0; si < n; si++) {
            OverviewSprite *sp = &sprites[si];
            int row = world_y - sp->y;
            int col = world_x - sp->x;
            if (row < 0 || row >= sp->h || col < 0 || col >= 8)
                continue;
            uint8_t flags = sp->flags;
            if (flags & 0x40) row = sp->h - 1 - row;
            int tile = sp->tile;
            if (sp->h == 16) tile &= 0xFE;
            int addr = (tile + (row >> 3)) * 16 + (row & 7) * 2;
            const uint8_t *tiles = (cgb_render_enabled && (flags & 0x08))
                                   ? vram_cgb1 : vram;
            int bit = (flags & 0x20) ? col : 7 - col;
            int ci = (((tiles[addr + 1] >> bit) & 1) << 1) |
                     ((tiles[addr] >> bit) & 1);
            if (ci == 0 || ((flags & 0x80) && bgci[x] != 0))
                continue;

            /* DMG SML2 uses lower screen X, then lower OAM index for priority. */
            int priority = sp->screen_x * 64 + sp->index;
            if (cgb_render_enabled && !(cgb_opri & 1))
                priority = sp->index;
            if (priority < best_priority) {
                best = si; best_priority = priority;
                best_ci = ci; best_addr = addr; best_row = row;
                best_col = col; best_flags = flags;
            }
        }
        if (best < 0) continue;
        uint8_t pal = (best_flags & 0x10) ? obp1 : obp0;
        if (cgb_render_enabled) {
            out[x] = (uint8_t)best_ci;
            rgb[x] = cgb_rgb(cgb_obj_palette, (unsigned)(best_flags & 7),
                             (unsigned)best_ci);
        } else {
            out[x] = (uint8_t)((pal >> (best_ci * 2)) & 3);
        }
        ppu_layer[y][x] = (best_flags & 0x10) ? 2 : 1;
        ppu_sprtile[y][x] = (uint16_t)(best_addr >> 4);
        ppu_spruv[y][x] = (uint8_t)(((best_row & 7) << 3) |
            ((best_flags & 0x20) ? (7 - best_col) : best_col) |
            ((best_flags & 0x20) ? 0x40 : 0) |
            ((best_flags & 0x40) ? 0x80 : 0));
    }
}

static void render_line(int y)
{
    if (y == 0) ppu_refresh_wide_layout();
    const int W = ppu_w, L = ppu_xoff;
    if (ppu_homeworld_overview) {
        render_homeworld_overview_line(y);
        return;
    }
    hud_lines = wide_on ? cfg_hud : 0; win_centre = wide_on ? cfg_win : 0;
    if (y == 0 && ppu_w > GB_W) {
        /* Captures were collected during the preceding game update. Snapshot
           them before starting the next frame's margin rendering. */
        if (wide_sml2_snapshot_actors) wide_sml2_snapshot_actors();
        if (wide_sml2_prepare_frame)
            wide_sml2_prepare_frame(L, wide_r, scx, scy, lcdc, wx, wy, vram);
    }
    int sml2_margin_mode = 0;
    if (wide_on && ppu_w > GB_W && wide_sml2_world_origin) {
        int origin_x, origin_y;
        sml2_margin_mode = wide_sml2_world_origin(&origin_x, &origin_y);
    }
    uint8_t *out = ppu_shade[y];
    uint32_t *rgb = ppu_rgb[y];
    memset(ppu_layer[y], 0, W);
    memset(ppu_bgtile[y], 0xFF, W * sizeof ppu_bgtile[y][0]);
    memset(ppu_sprtile[y], 0xFF, W * sizeof ppu_sprtile[y][0]);
    uint8_t bgci[GB_WMAX], bgattr[GB_WMAX];
    memset(bgci, 0, sizeof bgci);
    memset(bgattr, 0, sizeof bgattr);

    /* On CGB, LCDC.0 enables BG/OBJ priority; it does not suppress BG pixels. */
    if (cgb_render_enabled || (lcdc & 0x01)) {
        int map_off = (lcdc & 0x08) ? 0x1C00 : 0x1800;
        int sy = (y + scy) & 0xFF;
        int off = L + (y < hud_lines ? hud_shift : 0);
        for (int X = 0; X < W; X++) {
            int px = (X - off + scx) & 0xFF, attr = 0, tile_id = 0;
            int ci = tile_pixel(map_off, px, sy, &attr, &tile_id);
            bgci[X] = (uint8_t)ci; bgattr[X] = (uint8_t)attr;
            out[X] = cgb_render_enabled ? (uint8_t)ci : (uint8_t)((bgp >> (ci * 2)) & 3);
            ppu_bgtile[y][X] = (uint16_t)tile_id;
            ppu_bguv[y][X] = (uint8_t)(((sy & 7) << 3) | (px & 7));
            if (cgb_render_enabled)
                rgb[X] = cgb_rgb(cgb_bg_palette, (unsigned)(attr & 7), (unsigned)ci);
        }
    } else {
        memset(out, 0, W);
        memset(rgb, 0, W * sizeof rgb[0]);
    }

    /* SML2's visible world is wider than its hardware tilemap ring. Rebuild
       only the margins from the live 16x16 block map, leaving the original
       160 columns untouched. Sprites and the native window keep their PPU path. */
    if (wide_on && W > GB_W && y < wy && wide_sml2_margin_tile) {
        for (int X = 0; X < W; X++) {
            if (X >= L && X < L + GB_W) continue;
            uint8_t tile = 0;
            int world_x = 0, world_y = 0;
            int margin = wide_sml2_margin_tile(X, y, &tile, &world_x, &world_y);
            if (margin == 0) continue;
            if (margin < 0) {
                bgci[X] = 0;
                bgattr[X] = 0;
                out[X] = cgb_render_enabled ? 0 : (uint8_t)(bgp & 3);
                if (cgb_render_enabled) rgb[X] = cgb_rgb(cgb_bg_palette, 0, 0);
                ppu_bgtile[y][X] = 0xFFFF;
                ppu_bguv[y][X] = 0;
                continue;
            }
            int attr_map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
            unsigned attr_index = (unsigned)attr_map +
                (unsigned)(((world_y >> 3) & 31) * 32) +
                (unsigned)((world_x >> 3) & 31);
            uint8_t world_attr = cgb_render_enabled ? vram_cgb1[attr_index] : 0;
            int attr = 0, tile_id = 0;
            int ci = tile_pixel_data(tile, world_attr, world_x, world_y, &attr, &tile_id);
            bgci[X] = (uint8_t)ci;
            bgattr[X] = (uint8_t)attr;
            out[X] = cgb_render_enabled ? (uint8_t)ci : (uint8_t)((bgp >> (ci * 2)) & 3);
            if (cgb_render_enabled)
                rgb[X] = cgb_rgb(cgb_bg_palette, (unsigned)(attr & 7), (unsigned)ci);
            ppu_bgtile[y][X] = (uint16_t)tile_id;
            ppu_bguv[y][X] = (uint8_t)(((world_y & 7) << 3) | (world_x & 7));
        }
    }

    if ((lcdc & 0x20) && (cgb_render_enabled || (lcdc & 0x01)) && y >= wy && wx <= 166) {
        int map_off = (lcdc & 0x40) ? 0x1C00 : 0x1800;
        int x0 = wx - 7 + L + (win_centre ? hud_shift : 0), drew = 0;
        int first = win_centre ? 0 : (x0 < 0 ? 0 : x0);
        for (int X = first; X < W; X++) {
            int px = (X - x0) & 0xFF, attr = 0, tile_id = 0;
            int ci = tile_pixel(map_off, px, wlc, &attr, &tile_id);
            bgci[X] = (uint8_t)ci; bgattr[X] = (uint8_t)attr;
            out[X] = cgb_render_enabled ? (uint8_t)ci : (uint8_t)((bgp >> (ci * 2)) & 3);
            ppu_bgtile[y][X] = (uint16_t)tile_id;
            ppu_bguv[y][X] = (uint8_t)(((wlc & 7) << 3) | (px & 7));
            if (cgb_render_enabled)
                rgb[X] = cgb_rgb(cgb_bg_palette, (unsigned)(attr & 7), (unsigned)ci);
            drew = 1;
        }
        if (drew) wlc++;
    }

    /* The ROM clips many SML2 metasprites at the native 160px edge. Paint
       only their out-of-centre pieces from the captured world-space list.
       The original PPU remains authoritative for every centre pixel. */
    if (wide_on && W > GB_W && y < wy &&
        wide_sml2_actor_sprites && wide_sml2_world_origin) {
        int count = 0, world_left = 0, world_top = 0;
        const WideSml2Sprite *sprites = wide_sml2_actor_sprites(&count);
        if (sprites && count > 0 && wide_sml2_world_origin(&world_left, &world_top)) {
            int h = (lcdc & 0x04) ? 16 : 8;
            for (int i = count - 1; i >= 0; i--) {
                const WideSml2Sprite *sp = &sprites[i];
                int sx = (int)sp->x - world_left;
                int sy = (int)sp->y - world_top;
                int row = y - sy;
                if (row < 0 || row >= h) continue;
                uint8_t fl = sp->attr;
                if (fl & 0x40) row = h - 1 - row;
                uint8_t tile = h == 16 ? (uint8_t)(sp->tile & 0xFEu) : sp->tile;
                int addr = ((int)tile + (row >> 3)) * 16 + (row & 7) * 2;
                const uint8_t *tiles = (cgb_render_enabled && (fl & 0x08)) ? vram_cgb1 : vram;
                uint8_t pal = (fl & 0x10) ? obp1 : obp0;
                for (int px = 0; px < 8; px++) {
                    int x = sx + px;
                    if (x < 0 || x >= W || (x >= L && x < L + GB_W)) continue;
                    int bit = (fl & 0x20) ? px : 7 - px;
                    int ci = (((tiles[addr + 1] >> bit) & 1) << 1) |
                             ((tiles[addr] >> bit) & 1);
                    if (ci == 0) continue;
                    if (cgb_render_enabled) {
                        if ((lcdc & 0x01) && bgci[x] != 0 &&
                            ((bgattr[x] & 0x80) || (fl & 0x80))) continue;
                        out[x] = (uint8_t)ci;
                        rgb[x] = cgb_rgb(cgb_obj_palette, (unsigned)(fl & 7), (unsigned)ci);
                    } else {
                        if ((fl & 0x80) && bgci[x] != 0) continue;
                        out[x] = (uint8_t)((pal >> (ci * 2)) & 3);
                    }
                    ppu_layer[y][x] = (fl & 0x10) ? 2 : 1;
                    ppu_sprtile[y][x] = (uint16_t)(addr >> 4);
                    ppu_spruv[y][x] = (uint8_t)(((row & 7) << 3) | (7 - bit) |
                                                 ((fl & 0x20) ? 0x40 : 0) |
                                                 ((fl & 0x40) ? 0x80 : 0));
                }
            }
        }
    }

    if (lcdc & 0x02) {
        int h = (lcdc & 0x04) ? 16 : 8;
        int idx[10], n = 0;
        for (int i = 0; i < 40 && n < 10; i++) {
            int sy = oam[i * 4] - 16;
            if (y >= sy && y < sy + h) idx[n++] = i;
        }
        /* DMG (and CGB OPRI=1): lower X wins. Native CGB mode defaults to OAM order. */
        if (!cgb_render_enabled || (cgb_opri & 1)) {
            for (int a = 0; a < n; a++)
                for (int b = a + 1; b < n; b++) {
                    int xa = oam[idx[a] * 4 + 1], xb = oam[idx[b] * 4 + 1];
                    if (xb > xa || (xb == xa && idx[b] > idx[a])) {
                        int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
                    }
                }
        }
        for (int kk = 0; kk < n; kk++) {
            int k = (cgb_render_enabled && !(cgb_opri & 1)) ? n - 1 - kk : kk;
            int i = idx[k];
            int ox = oam[i * 4 + 1];
            if (ox >= sprite_neg) ox -= 256;
            int sy = oam[i * 4] - 16, sx = ox - 8 + L;
            uint8_t tile = oam[i * 4 + 2], fl = oam[i * 4 + 3];
            int row = y - sy;
            if (fl & 0x40) row = h - 1 - row;
            if (h == 16) tile &= 0xFE;
            int addr = ((int)tile + (row >> 3)) * 16 + (row & 7) * 2;
            const uint8_t *tiles = (cgb_render_enabled && (fl & 0x08)) ? vram_cgb1 : vram;
            uint8_t pal = (fl & 0x10) ? obp1 : obp0;
            for (int px = 0; px < 8; px++) {
                int x = sx + px;
                if (x < 0 || x >= W) continue;
                /* Wrapped OAM coordinates are not authoritative outside the
                   native strip; true-world captures above supply those pixels. */
                if (sml2_margin_mode && (x < L || x >= L + GB_W)) continue;
                int bit = (fl & 0x20) ? px : 7 - px;
                int ci = (((tiles[addr + 1] >> bit) & 1) << 1) | ((tiles[addr] >> bit) & 1);
                if (ci == 0) continue;
                if (cgb_render_enabled) {
                    if ((lcdc & 0x01) && bgci[x] != 0 && ((bgattr[x] & 0x80) || (fl & 0x80)))
                        continue;
                    out[x] = (uint8_t)ci;
                    rgb[x] = cgb_rgb(cgb_obj_palette, (unsigned)(fl & 7), (unsigned)ci);
                    ppu_layer[y][x] = 1;
                } else {
                    if ((fl & 0x80) && bgci[x] != 0) continue;
                    out[x] = (pal >> (ci * 2)) & 3;
                    ppu_layer[y][x] = (fl & 0x10) ? 2 : 1;
                }
                ppu_sprtile[y][x] = (uint16_t)(addr >> 4);
                ppu_spruv[y][x] = (uint8_t)(((row & 7) << 3) | (7 - bit) |
                                             ((fl & 0x20) ? 0x40 : 0) |
                                             ((fl & 0x40) ? 0x80 : 0));
            }
        }
    }
    if (!wide_on) {
        for (int X = 0; X < W; X++) {
            if (X >= L && X < L + GB_W) continue;
            out[X] = 3; rgb[X] = 0;
            ppu_layer[y][X] = 0; ppu_bgtile[y][X] = 0xFFFF; ppu_sprtile[y][X] = 0xFFFF;
        }
    }
}
void ppu_tick(int n)
{
    if (!lcd_on) {
        off_cycles += n;
        if (off_cycles >= CYCLES_PER_FRAME) { off_cycles -= CYCLES_PER_FRAME; frame_count++; frame_hook(); }
        return;
    }
    dot += n;
    for (;;) {
        if (ly < 144) {
            if (mode == 2) { if (dot < 80) break; mode = 3; update_stat(); continue; }
            if (mode == 3) { if (dot < 252) break; mode = 0; render_line(ly); update_stat(); continue; }
            if (dot < 456) break;
            dot -= 456; ly++;
            if (ly == 144) {
                mode = 1; io_if |= 0x01; wlc = 0;
                update_stat();
                frame_count++;
                frame_hook();
            } else { mode = 2; update_stat(); }
        } else {
            if (dot < 456) break;
            dot -= 456; ly++;
            if (ly == 154) {
                ly = 0; mode = 2;
                wide_on = wide_gate == 0 ? 1 : (wide_gate == 1 ? (io_ie & 4) != 0 : ((lcdc & 0x20) && wx == 7 && wy == 0x88));
            }
            update_stat();
        }
    }
}

#define GB_WMAX_LEGACY 256
#define GB_WMAX_PREVIOUS 320
#define GB_WMAX_384 384
typedef struct {
    uint8_t vram[0x2000], vram_cgb1[0x2000], vram_bank, oam[0xA0];
    uint8_t cgb_bg_palette[64], cgb_obj_palette[64], cgb_bgpi, cgb_obpi, cgb_opri;
    uint8_t shade[GB_H][GB_WMAX], layer[GB_H][GB_WMAX];
    uint32_t rgb[GB_H][GB_WMAX];
    int cgb_render_enabled;
    uint16_t bgtile[GB_H][GB_WMAX], sprtile[GB_H][GB_WMAX];
    uint8_t bguv[GB_H][GB_WMAX], spruv[GB_H][GB_WMAX];
    int ppu_w, ppu_xoff;
    int hud_lines, hud_shift, wide_r, sprite_neg, win_centre, wide_gate, wide_on, cfg_hud, cfg_win;
    int cfg_left, cfg_right;
    uint8_t lcdc, stat_sel, scy, scx, lyc, bgp, obp0, obp1, wy, wx;
    int dot, ly, mode, lcd_on, stat_line, wlc, off_cycles, frame_count;
} PPUState;

/* Exact layout used by the 384px-wide Homeworld build for old save states. */
typedef struct {
    uint8_t vram[0x2000], vram_cgb1[0x2000], vram_bank, oam[0xA0];
    uint8_t cgb_bg_palette[64], cgb_obj_palette[64], cgb_bgpi, cgb_obpi, cgb_opri;
    uint8_t shade[GB_H][GB_WMAX_384], layer[GB_H][GB_WMAX_384];
    uint32_t rgb[GB_H][GB_WMAX_384];
    int cgb_render_enabled;
    uint16_t bgtile[GB_H][GB_WMAX_384], sprtile[GB_H][GB_WMAX_384];
    uint8_t bguv[GB_H][GB_WMAX_384], spruv[GB_H][GB_WMAX_384];
    int ppu_w, ppu_xoff;
    int hud_lines, hud_shift, wide_r, sprite_neg, win_centre, wide_gate, wide_on, cfg_hud, cfg_win;
    int cfg_left, cfg_right;
    uint8_t lcdc, stat_sel, scy, scx, lyc, bgp, obp0, obp1, wy, wx;
    int dot, ly, mode, lcd_on, stat_line, wlc, off_cycles, frame_count;
} PPUState384;

/* Exact layout used by the 320px-wide Homeworld build for old save states. */
typedef struct {
    uint8_t vram[0x2000], vram_cgb1[0x2000], vram_bank, oam[0xA0];
    uint8_t cgb_bg_palette[64], cgb_obj_palette[64], cgb_bgpi, cgb_obpi, cgb_opri;
    uint8_t shade[GB_H][GB_WMAX_PREVIOUS], layer[GB_H][GB_WMAX_PREVIOUS];
    uint32_t rgb[GB_H][GB_WMAX_PREVIOUS];
    int cgb_render_enabled;
    uint16_t bgtile[GB_H][GB_WMAX_PREVIOUS], sprtile[GB_H][GB_WMAX_PREVIOUS];
    uint8_t bguv[GB_H][GB_WMAX_PREVIOUS], spruv[GB_H][GB_WMAX_PREVIOUS];
    int ppu_w, ppu_xoff;
    int hud_lines, hud_shift, wide_r, sprite_neg, win_centre, wide_gate, wide_on, cfg_hud, cfg_win;
    int cfg_left, cfg_right;
    uint8_t lcdc, stat_sel, scy, scx, lyc, bgp, obp0, obp1, wy, wx;
    int dot, ly, mode, lcd_on, stat_line, wlc, off_cycles, frame_count;
} PPUStatePrevious;


/* Exact layout used while GB_WMAX was 256, for loading existing save states. */
typedef struct {
    uint8_t vram[0x2000], vram_cgb1[0x2000], vram_bank, oam[0xA0];
    uint8_t cgb_bg_palette[64], cgb_obj_palette[64], cgb_bgpi, cgb_obpi, cgb_opri;
    uint8_t shade[GB_H][GB_WMAX_LEGACY], layer[GB_H][GB_WMAX_LEGACY];
    uint32_t rgb[GB_H][GB_WMAX_LEGACY];
    int cgb_render_enabled;
    uint16_t bgtile[GB_H][GB_WMAX_LEGACY], sprtile[GB_H][GB_WMAX_LEGACY];
    uint8_t bguv[GB_H][GB_WMAX_LEGACY], spruv[GB_H][GB_WMAX_LEGACY];
    int ppu_w, ppu_xoff;
    int hud_lines, hud_shift, wide_r, sprite_neg, win_centre, wide_gate, wide_on, cfg_hud, cfg_win;
    uint8_t lcdc, stat_sel, scy, scx, lyc, bgp, obp0, obp1, wy, wx;
    int dot, ly, mode, lcd_on, stat_line, wlc, off_cycles, frame_count;
} PPUStateLegacy;

size_t ppu_state_size(void) { return sizeof(PPUState); }
int ppu_state_size_compatible(size_t n)
{
    return n == sizeof(PPUState) || n == sizeof(PPUState384) ||
           n == sizeof(PPUStatePrevious) || n == sizeof(PPUStateLegacy);
}
int ppu_state_save(void *dst, size_t n)
{
    if (!dst || n < sizeof(PPUState)) return -1;
    PPUState *s = (PPUState *)dst;
    memcpy(s->vram,vram,sizeof vram); memcpy(s->vram_cgb1,vram_cgb1,sizeof vram_cgb1); s->vram_bank=vram_bank; memcpy(s->cgb_bg_palette,cgb_bg_palette,sizeof cgb_bg_palette); memcpy(s->cgb_obj_palette,cgb_obj_palette,sizeof cgb_obj_palette); s->cgb_bgpi=cgb_bgpi; s->cgb_obpi=cgb_obpi; s->cgb_opri=cgb_opri; memcpy(s->oam,oam,sizeof oam);
    memcpy(s->shade,ppu_shade,sizeof ppu_shade); memcpy(s->rgb,ppu_rgb,sizeof ppu_rgb); s->cgb_render_enabled=cgb_render_enabled; memcpy(s->layer,ppu_layer,sizeof ppu_layer);
    memcpy(s->bgtile,ppu_bgtile,sizeof ppu_bgtile); memcpy(s->sprtile,ppu_sprtile,sizeof ppu_sprtile);
    memcpy(s->bguv,ppu_bguv,sizeof ppu_bguv); memcpy(s->spruv,ppu_spruv,sizeof ppu_spruv);
    s->ppu_w=ppu_w; s->ppu_xoff=ppu_xoff; s->hud_lines=hud_lines; s->hud_shift=hud_shift; s->wide_r=wide_r; s->sprite_neg=sprite_neg;
    s->win_centre=win_centre; s->wide_gate=wide_gate; s->wide_on=wide_on; s->cfg_hud=cfg_hud; s->cfg_win=cfg_win;
    s->cfg_left=cfg_left; s->cfg_right=cfg_right;
    s->lcdc=lcdc; s->stat_sel=stat_sel; s->scy=scy; s->scx=scx; s->lyc=lyc; s->bgp=bgp; s->obp0=obp0; s->obp1=obp1; s->wy=wy; s->wx=wx;
    s->dot=dot; s->ly=ly; s->mode=mode; s->lcd_on=lcd_on; s->stat_line=stat_line; s->wlc=wlc; s->off_cycles=off_cycles; s->frame_count=frame_count;
    return 0;
}
int ppu_state_load(const void *src, size_t n)
{
    if (!src) return -1;
    if (n == sizeof(PPUState384)) {
        const PPUState384 *old = (const PPUState384 *)src;
        memcpy(vram, old->vram, sizeof vram);
        memcpy(vram_cgb1, old->vram_cgb1, sizeof vram_cgb1);
        vram_bank = old->vram_bank & 1; memcpy(oam, old->oam, sizeof oam);
        memcpy(cgb_bg_palette, old->cgb_bg_palette, sizeof cgb_bg_palette);
        memcpy(cgb_obj_palette, old->cgb_obj_palette, sizeof cgb_obj_palette);
        cgb_bgpi=old->cgb_bgpi; cgb_obpi=old->cgb_obpi; cgb_opri=old->cgb_opri;
        for (int y=0; y<GB_H; y++) {
            memcpy(ppu_shade[y], old->shade[y], GB_WMAX_384);
            memset(ppu_shade[y]+GB_WMAX_384, 0, GB_WMAX-GB_WMAX_384);
            memcpy(ppu_layer[y], old->layer[y], GB_WMAX_384);
            memset(ppu_layer[y]+GB_WMAX_384, 0, GB_WMAX-GB_WMAX_384);
            memcpy(ppu_rgb[y], old->rgb[y], GB_WMAX_384*sizeof old->rgb[y][0]);
            memset(ppu_rgb[y]+GB_WMAX_384, 0, (GB_WMAX-GB_WMAX_384)*sizeof ppu_rgb[y][0]);
            memcpy(ppu_bgtile[y], old->bgtile[y], GB_WMAX_384*sizeof old->bgtile[y][0]);
            memset(ppu_bgtile[y]+GB_WMAX_384, 0xFF, (GB_WMAX-GB_WMAX_384)*sizeof ppu_bgtile[y][0]);
            memcpy(ppu_sprtile[y], old->sprtile[y], GB_WMAX_384*sizeof old->sprtile[y][0]);
            memset(ppu_sprtile[y]+GB_WMAX_384, 0xFF, (GB_WMAX-GB_WMAX_384)*sizeof ppu_sprtile[y][0]);
            memcpy(ppu_bguv[y], old->bguv[y], GB_WMAX_384);
            memset(ppu_bguv[y]+GB_WMAX_384, 0, GB_WMAX-GB_WMAX_384);
            memcpy(ppu_spruv[y], old->spruv[y], GB_WMAX_384);
            memset(ppu_spruv[y]+GB_WMAX_384, 0, GB_WMAX-GB_WMAX_384);
        }
        cgb_render_enabled=old->cgb_render_enabled!=0;
        ppu_w=old->ppu_w; ppu_xoff=old->ppu_xoff; hud_lines=old->hud_lines;
        hud_shift=old->hud_shift; wide_r=old->wide_r; sprite_neg=old->sprite_neg;
        win_centre=old->win_centre; wide_gate=old->wide_gate; wide_on=old->wide_on;
        cfg_hud=old->cfg_hud; cfg_win=old->cfg_win;
        cfg_left=old->cfg_left; cfg_right=old->cfg_right;
        lcdc=old->lcdc; stat_sel=old->stat_sel; scy=old->scy; scx=old->scx; lyc=old->lyc;
        bgp=old->bgp; obp0=old->obp0; obp1=old->obp1; wy=old->wy; wx=old->wx;
        dot=old->dot; ly=old->ly; mode=old->mode; lcd_on=old->lcd_on; stat_line=old->stat_line;
        wlc=old->wlc; off_cycles=old->off_cycles; frame_count=old->frame_count;
        return 0;
    }
    if (n == sizeof(PPUStatePrevious)) {
        const PPUStatePrevious *old = (const PPUStatePrevious *)src;
        memcpy(vram, old->vram, sizeof vram);
        memcpy(vram_cgb1, old->vram_cgb1, sizeof vram_cgb1);
        vram_bank = old->vram_bank & 1; memcpy(oam, old->oam, sizeof oam);
        memcpy(cgb_bg_palette, old->cgb_bg_palette, sizeof cgb_bg_palette);
        memcpy(cgb_obj_palette, old->cgb_obj_palette, sizeof cgb_obj_palette);
        cgb_bgpi=old->cgb_bgpi; cgb_obpi=old->cgb_obpi; cgb_opri=old->cgb_opri;
        for (int y=0; y<GB_H; y++) {
            memcpy(ppu_shade[y], old->shade[y], GB_WMAX_PREVIOUS);
            memset(ppu_shade[y]+GB_WMAX_PREVIOUS, 0, GB_WMAX-GB_WMAX_PREVIOUS);
            memcpy(ppu_layer[y], old->layer[y], GB_WMAX_PREVIOUS);
            memset(ppu_layer[y]+GB_WMAX_PREVIOUS, 0, GB_WMAX-GB_WMAX_PREVIOUS);
            memcpy(ppu_rgb[y], old->rgb[y], GB_WMAX_PREVIOUS*sizeof old->rgb[y][0]);
            memset(ppu_rgb[y]+GB_WMAX_PREVIOUS, 0, (GB_WMAX-GB_WMAX_PREVIOUS)*sizeof ppu_rgb[y][0]);
            memcpy(ppu_bgtile[y], old->bgtile[y], GB_WMAX_PREVIOUS*sizeof old->bgtile[y][0]);
            memset(ppu_bgtile[y]+GB_WMAX_PREVIOUS, 0xFF, (GB_WMAX-GB_WMAX_PREVIOUS)*sizeof ppu_bgtile[y][0]);
            memcpy(ppu_sprtile[y], old->sprtile[y], GB_WMAX_PREVIOUS*sizeof old->sprtile[y][0]);
            memset(ppu_sprtile[y]+GB_WMAX_PREVIOUS, 0xFF, (GB_WMAX-GB_WMAX_PREVIOUS)*sizeof ppu_sprtile[y][0]);
            memcpy(ppu_bguv[y], old->bguv[y], GB_WMAX_PREVIOUS);
            memset(ppu_bguv[y]+GB_WMAX_PREVIOUS, 0, GB_WMAX-GB_WMAX_PREVIOUS);
            memcpy(ppu_spruv[y], old->spruv[y], GB_WMAX_PREVIOUS);
            memset(ppu_spruv[y]+GB_WMAX_PREVIOUS, 0, GB_WMAX-GB_WMAX_PREVIOUS);
        }
        cgb_render_enabled=old->cgb_render_enabled!=0;
        ppu_w=old->ppu_w; ppu_xoff=old->ppu_xoff; hud_lines=old->hud_lines;
        hud_shift=old->hud_shift; wide_r=old->wide_r; sprite_neg=old->sprite_neg;
        win_centre=old->win_centre; wide_gate=old->wide_gate; wide_on=old->wide_on;
        cfg_hud=old->cfg_hud; cfg_win=old->cfg_win;
        cfg_left=old->cfg_left; cfg_right=old->cfg_right;
        lcdc=old->lcdc; stat_sel=old->stat_sel; scy=old->scy; scx=old->scx; lyc=old->lyc;
        bgp=old->bgp; obp0=old->obp0; obp1=old->obp1; wy=old->wy; wx=old->wx;
        dot=old->dot; ly=old->ly; mode=old->mode; lcd_on=old->lcd_on; stat_line=old->stat_line;
        wlc=old->wlc; off_cycles=old->off_cycles; frame_count=old->frame_count;
        return 0;
    }
    if (n == sizeof(PPUStateLegacy)) {
        const PPUStateLegacy *old = (const PPUStateLegacy *)src;
        memcpy(vram, old->vram, sizeof vram);
        memcpy(vram_cgb1, old->vram_cgb1, sizeof vram_cgb1);
        vram_bank = old->vram_bank & 1; memcpy(oam, old->oam, sizeof oam);
        memcpy(cgb_bg_palette, old->cgb_bg_palette, sizeof cgb_bg_palette);
        memcpy(cgb_obj_palette, old->cgb_obj_palette, sizeof cgb_obj_palette);
        cgb_bgpi=old->cgb_bgpi; cgb_obpi=old->cgb_obpi; cgb_opri=old->cgb_opri;
        for (int y=0; y<GB_H; y++) {
            memcpy(ppu_shade[y], old->shade[y], GB_WMAX_LEGACY);
            memset(ppu_shade[y]+GB_WMAX_LEGACY, 0, GB_WMAX-GB_WMAX_LEGACY);
            memcpy(ppu_layer[y], old->layer[y], GB_WMAX_LEGACY);
            memset(ppu_layer[y]+GB_WMAX_LEGACY, 0, GB_WMAX-GB_WMAX_LEGACY);
            memcpy(ppu_rgb[y], old->rgb[y], GB_WMAX_LEGACY*sizeof old->rgb[y][0]);
            memset(ppu_rgb[y]+GB_WMAX_LEGACY, 0, (GB_WMAX-GB_WMAX_LEGACY)*sizeof ppu_rgb[y][0]);
            memcpy(ppu_bgtile[y], old->bgtile[y], GB_WMAX_LEGACY*sizeof old->bgtile[y][0]);
            memset(ppu_bgtile[y]+GB_WMAX_LEGACY, 0xFF, (GB_WMAX-GB_WMAX_LEGACY)*sizeof ppu_bgtile[y][0]);
            memcpy(ppu_sprtile[y], old->sprtile[y], GB_WMAX_LEGACY*sizeof old->sprtile[y][0]);
            memset(ppu_sprtile[y]+GB_WMAX_LEGACY, 0xFF, (GB_WMAX-GB_WMAX_LEGACY)*sizeof ppu_sprtile[y][0]);
            memcpy(ppu_bguv[y], old->bguv[y], GB_WMAX_LEGACY);
            memset(ppu_bguv[y]+GB_WMAX_LEGACY, 0, GB_WMAX-GB_WMAX_LEGACY);
            memcpy(ppu_spruv[y], old->spruv[y], GB_WMAX_LEGACY);
            memset(ppu_spruv[y]+GB_WMAX_LEGACY, 0, GB_WMAX-GB_WMAX_LEGACY);
        }
        cgb_render_enabled=old->cgb_render_enabled!=0;
        ppu_w=old->ppu_w; ppu_xoff=old->ppu_xoff; hud_lines=old->hud_lines;
        hud_shift=old->hud_shift; wide_r=old->wide_r; sprite_neg=old->sprite_neg;
        win_centre=old->win_centre; wide_gate=old->wide_gate; wide_on=old->wide_on;
        cfg_hud=old->cfg_hud; cfg_win=old->cfg_win;
        cfg_left=old->ppu_xoff; cfg_right=old->wide_r;
        lcdc=old->lcdc; stat_sel=old->stat_sel; scy=old->scy; scx=old->scx; lyc=old->lyc;
        bgp=old->bgp; obp0=old->obp0; obp1=old->obp1; wy=old->wy; wx=old->wx;
        dot=old->dot; ly=old->ly; mode=old->mode; lcd_on=old->lcd_on; stat_line=old->stat_line;
        wlc=old->wlc; off_cycles=old->off_cycles; frame_count=old->frame_count;
        return 0;
    }
    if (n < sizeof(PPUState)) return -1;
    const PPUState *s = (const PPUState *)src;
    memcpy(vram,s->vram,sizeof vram); memcpy(vram_cgb1,s->vram_cgb1,sizeof vram_cgb1); vram_bank=s->vram_bank & 1; memcpy(cgb_bg_palette,s->cgb_bg_palette,sizeof cgb_bg_palette); memcpy(cgb_obj_palette,s->cgb_obj_palette,sizeof cgb_obj_palette); cgb_bgpi=s->cgb_bgpi; cgb_obpi=s->cgb_obpi; cgb_opri=s->cgb_opri; memcpy(oam,s->oam,sizeof oam);
    memcpy(ppu_shade,s->shade,sizeof ppu_shade); memcpy(ppu_rgb,s->rgb,sizeof ppu_rgb); cgb_render_enabled=s->cgb_render_enabled != 0; memcpy(ppu_layer,s->layer,sizeof ppu_layer);
    memcpy(ppu_bgtile,s->bgtile,sizeof ppu_bgtile); memcpy(ppu_sprtile,s->sprtile,sizeof ppu_sprtile);
    memcpy(ppu_bguv,s->bguv,sizeof ppu_bguv); memcpy(ppu_spruv,s->spruv,sizeof ppu_spruv);
    ppu_w=s->ppu_w; ppu_xoff=s->ppu_xoff; hud_lines=s->hud_lines; hud_shift=s->hud_shift; wide_r=s->wide_r; sprite_neg=s->sprite_neg;
    win_centre=s->win_centre; wide_gate=s->wide_gate; wide_on=s->wide_on; cfg_hud=s->cfg_hud; cfg_win=s->cfg_win;
    cfg_left=s->cfg_left; cfg_right=s->cfg_right;
    lcdc=s->lcdc; stat_sel=s->stat_sel; scy=s->scy; scx=s->scx; lyc=s->lyc; bgp=s->bgp; obp0=s->obp0; obp1=s->obp1; wy=s->wy; wx=s->wx;
    dot=s->dot; ly=s->ly; mode=s->mode; lcd_on=s->lcd_on; stat_line=s->stat_line; wlc=s->wlc; off_cycles=s->off_cycles; frame_count=s->frame_count;
    return 0;
}

int ppu_lcd_is_on(void) { return lcd_on; }