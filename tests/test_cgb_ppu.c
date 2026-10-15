#include "gb.h"
#include "cart.h"
#include "widescreen.h"
#include <assert.h>

/* This renderer-only test links ppu.c without the cartridge module. */
uint8_t *rom;
uint8_t *cart_ram;
static CartInfo renderer_test_cart;
const CartInfo *cart_info(void) { return &renderer_test_cart; }

uint8_t io_if, io_ie;
void frame_hook(void) {}

static uint8_t wl2_test_rom[0x200000];
static uint8_t wl2_test_ram[0x8000];
static int wl2_test_active;

/* Keep the WL2 frame camera at world origin zero unless SCX asks for a
   deliberate shift in the ring-to-world mapping. */
uint8_t rd8(uint16_t address)
{
    switch (address) {
    case 0xD53C: return 0x00; /* GBC player X high */
    case 0xD53D: return 0x80; /* GBC player X low */
    case 0xD53A: return 0x00; /* GBC player Y high */
    case 0xD53B: return 0x80; /* GBC player Y low */
    case 0xC660: return 0x80; /* camera X */
    case 0xC65F: return 0x80; /* camera Y */
    case 0xC704: return 0x12; /* non-null tile reference */
    case 0xC705: return 0x34;
    case 0xD510: return 1;    /* stable level ID */
    default: return 0;
    }
}

int wide_wl2_block_at(int world_x, int world_y, uint8_t *block)
{
    if (!wl2_test_active || !block || world_x < 0 || world_y < 0)
        return 0;
    *block = (uint8_t)(1u + (((unsigned)world_x >> 4) & 7u) +
                      ((((unsigned)world_y >> 4) & 7u) << 3));
    return 1;
}

int wide_wl2_block_tile(int table_set, int block, int part, uint8_t *tile)
{
    if (!wl2_test_active || !rom || !tile || table_set < 0 || table_set >= 32 ||
        block < 0 || block >= 128 || part < 0 || part >= 4)
        return 0;
    size_t offset = 0x18000u + (size_t)table_set * 0x200u +
                    (size_t)block * 4u + (size_t)part;
    *tile = rom[offset];
    return 1;
}

int wide_wl2_block_attr(int table_set, int block, int part, uint8_t *attr)
{
    if (!wl2_test_active || !attr || table_set < 0 || table_set >= 32 ||
        block < 0 || block >= 128 || part < 0 || part >= 4)
        return 0;
    /* Synthetic GBC ROM table: palette 2 is authoritative for these blocks. */
    *attr = 2;
    return 1;
}


static int wl3_test_active;
static int wl3_test_camera_x = 16, wl3_test_camera_y;

int wide_wl3_block_at(int world_x, int world_y, uint8_t *block)
{
    if (!wl3_test_active || !block) return 0;
    if (world_x < 0 || world_y < 0 || world_x >= 2560 || world_y >= 1536)
        return -1;
    *block = 1;
    return 1;
}

int wide_wl3_block_tile(int block, int part, uint8_t *tile)
{
    if (!wl3_test_active || block != 1 || part < 0 || part >= 4 || !tile)
        return 0;
    *tile = 1;
    return 1;
}

int wide_wl3_block_attr(int block, int part, uint8_t *attr)
{
    if (!wl3_test_active || block != 1 || part < 0 || part >= 4 || !attr)
        return 0;
    *attr = 0;
    return 1;
}

int wide_wl3_camera(int *camera_x, int *camera_y)
{
    if (!wl3_test_active || !camera_x || !camera_y) return 0;
    *camera_x = wl3_test_camera_x;
    *camera_y = wl3_test_camera_y;
    return 1;
}

static void set_wl2_test_map(int stale_right, int special_visible_palette)
{
    /* This synthetic room uses palette 2 normally. Columns 20-23 can be
       deliberately left with another tile/palette as a recycled ring slot. */
    ppu_vram_bank_write(0);
    for (unsigned y = 0; y < 32; y++) {
        for (unsigned x = 0; x < 32; x++) {
            int stale = stale_right && x >= 20 && x < 24;
            ppu_vram_write((uint16_t)(0x1800u + y * 32u + x),
                           (uint8_t)(stale ? 2 : 1));
        }
    }
    ppu_vram_bank_write(1);
    for (unsigned y = 0; y < 32; y++) {
        for (unsigned x = 0; x < 32; x++) {
            int stale = stale_right && x >= 20 && x < 24;
            int special = special_visible_palette && x >= 20 && x < 24;
            uint8_t attr = stale ? 1 : (special ? 3 : 2);
            ppu_vram_write((uint16_t)(0x1800u + y * 32u + x), attr);
        }
    }
    ppu_vram_bank_write(0);
}

static void render_next_wl2_test_frame(void)
{
    /* The current test frame has already rendered scanline zero at dot 252. */
    ppu_tick(CYCLES_PER_FRAME - 252);
    ppu_tick(252);
}

static void set_bg_color(unsigned index, unsigned rgb555)
{
    ppu_cgb_write(0x68, (uint8_t)(0x80 | (index * 2)));
    ppu_cgb_write(0x69, (uint8_t)rgb555);
    ppu_cgb_write(0x69, (uint8_t)(rgb555 >> 8));
}

static void set_obj_color(unsigned palette, unsigned index, unsigned rgb555)
{
    unsigned off = palette * 8u + index * 2u;
    ppu_cgb_write(0x6A, (uint8_t)(0x80 | off));
    ppu_cgb_write(0x6B, (uint8_t)rgb555);
    ppu_cgb_write(0x6B, (uint8_t)(rgb555 >> 8));
}

int main(void)
{
    ppu_reset();
    ppu_set_cgb_mode(1);
    ppu_vram_bank_write(0);
    ppu_vram_write(0x0000, 0xFF);
    ppu_vram_write(0x0001, 0x00);
    ppu_vram_write(0x1800, 0x00);
    ppu_vram_bank_write(1);
    ppu_vram_write(0x0000, 0x00);
    ppu_vram_write(0x0001, 0xFF);
    ppu_vram_write(0x1800, 0x08);
    uint8_t bank1_snapshot[0x1800];
    ppu_vram_bank1_copy(bank1_snapshot);
    assert(bank1_snapshot[0] == 0x00);
    assert(bank1_snapshot[1] == 0xFF);
    ppu_vram_bank_write(0);
    set_bg_color(1, 0x001F); /* red */
    set_bg_color(2, 0x03E0); /* green */
    set_obj_color(3, 1, 0x03E0); /* OBJ palette 3, color 1 = green */
    uint32_t obj_palette[32];
    ppu_cgb_obj_palette_copy(obj_palette);
    assert(obj_palette[3 * 4 + 1] == 0x00FF00u);
    ppu_write(0x40, 0x91);
    ppu_tick(252);
    assert(ppu_rgb[0][0] == 0x00FF00u);

    ppu_reset();
    ppu_set_cgb_mode(0);
    ppu_vram_bank_write(0);
    ppu_vram_write(0x0000, 0xFF);
    ppu_vram_write(0x0001, 0x00);
    ppu_vram_write(0x1800, 0x00);
    ppu_write(0x47, 0xE4);
    ppu_write(0x40, 0x91);
    ppu_tick(252);
    assert(ppu_shade[0][0] == 1);

    /* Homeworld must keep the same native-scale viewport as regular SML2
       gameplay. Widescreen adds side margins; it must not shrink the 512px map
       into a tiny overview above the HUD when WX/WY identify the Homeworld. */
    ppu_reset();
    ppu_set_cgb_mode(0);
    ppu_set_wide(49, 33, 0, 0, 0);
    ppu_write(0x43, 64);   /* SCX */
    ppu_write(0x4A, 136);  /* WY */
    ppu_write(0x4B, 80);   /* WX */
    ppu_write(0x40, 0xE3); /* Homeworld LCDC/window setup */
    ppu_tick(252);
    assert(ppu_w == 242);
    assert(ppu_xoff == 49);
    ppu_tick(11 * 456);
    assert(ppu_w == 242);
    assert(ppu_xoff == 49);
    ppu_tick((135 - 11) * 456);
    assert(ppu_w == 242);
    assert(ppu_xoff == 49);

    /* Returning to SML2's regular window layout keeps the configured width. */
    ppu_write(0x4B, 7);
    ppu_tick(70224);
    assert(ppu_w == 242);
    assert(ppu_xoff == 49);

    /* Wario Land II GBC margin colors must not follow recycled palette data
       from a tilemap-ring slot that no longer contains the requested tile.
       First, force the right margin's map slot to hold tile 2 / palette 1 while
       the real block uses tile 1 / palette 2. It should stay green, not red. */
    memset(wl2_test_rom, 0, sizeof wl2_test_rom);
    memset(wl2_test_ram, 0, sizeof wl2_test_ram);
    for (unsigned block = 1; block < 128; block++)
        for (unsigned part = 0; part < 4; part++)
            wl2_test_rom[0x18000u + block * 4u + part] = 1;
    wl2_test_rom[0x143] = 0x80; /* GBC-compatible cartridge */
    rom = wl2_test_rom;
    cart_ram = wl2_test_ram;
    renderer_test_cart.size = sizeof wl2_test_rom;
    renderer_test_cart.ram_bytes = sizeof wl2_test_ram;
    renderer_test_cart.crc = 0x047BDF80u;
    wl2_test_active = 1;

    ppu_reset();
    ppu_set_cgb_mode(1);
    ppu_set_wide(32, 32, 0, 0, 0);
    ppu_set_wl2_wide(1);
    ppu_vram_bank_write(0);
    /* Tile 1 and tile 2 are both color-index 1; only the CGB attribute differs. */
    ppu_vram_write(0x0010, 0xFF);
    ppu_vram_write(0x0011, 0x00);
    ppu_vram_write(0x0020, 0xFF);
    ppu_vram_write(0x0021, 0x00);
    set_bg_color(5, 0x001F); /* palette 1 / color 1 = red */
    set_bg_color(9, 0x03E0); /* palette 2 / color 1 = green */
    set_bg_color(13, 0x7C00); /* palette 3 / color 1 = blue */
    set_wl2_test_map(1, 0);
    /* The first tile of the offscreen 16x16 block happens to match the
       selected ROM tile ID, but its three neighboring tilemap entries are
       stale from another part of the room. The old per-tile check accepted
       its wrong red palette and created a tiny color pop-in. */
    ppu_vram_bank_write(0);
    ppu_vram_write((uint16_t)(0x1800u + 20u), 1); /* coincidental tile match */
    ppu_vram_bank_write(1);
    ppu_vram_write((uint16_t)(0x1800u + 20u), 1); /* stale red palette */
    ppu_vram_bank_write(0);
    ppu_write(0x42, 0);
    ppu_write(0x43, 0);
    ppu_write(0x40, 0x91);
    ppu_tick(252);
    assert(ppu_rgb[0][192] == 0x00FF00u);

    /* Place world X=160 at the clipped right edge of the native viewport:
       only five pixels of its 16px block are visible. Even if that live map
       slot uses another palette, the ROM's static per-metatile palette remains
       the source of truth for this background block. */
    set_wl2_test_map(0, 1);
    ppu_write(0x43, 5);
    render_next_wl2_test_frame();
    set_wl2_test_map(1, 0);
    ppu_write(0x43, 0);
    render_next_wl2_test_frame();
    assert(ppu_rgb[0][192] == 0x00FF00u);
    wl2_test_active = 0;
    ppu_set_wl2_wide(0);

    /* Wario Land 3 rebuilds both side margins from its 160x96 block map,
       active room tile table and CGB attribute table. Validate a world origin
       near the left boundary: the part outside the level stays blank, while
       the in-level left and right margins show the expected tile. */
    wl3_test_active = 1;
    wl3_test_camera_x = 16;
    wl3_test_camera_y = 0;
    ppu_reset();
    ppu_set_cgb_mode(1);
    ppu_set_wide(32, 32, 0, 0, 0);
    ppu_set_wl3_wide(1);
    ppu_vram_bank_write(0);
    for (int row = 0; row < 8; row++) {
        ppu_vram_write((uint16_t)(0x0010 + row * 2), 0xFF);
        ppu_vram_write((uint16_t)(0x0011 + row * 2), 0x00);
    }
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            ppu_vram_write((uint16_t)(0x1800u + y * 32u + x), 1);
    ppu_vram_bank_write(1);
    for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
            ppu_vram_write((uint16_t)(0x1800u + y * 32u + x), 0);
    ppu_vram_bank_write(0);
    set_bg_color(0, 0x0000); /* outside-level background is black */
    set_bg_color(1, 0x001F); /* block color 1 is red */
    ppu_write(0x42, 0);
    ppu_write(0x43, 16);
    ppu_write(0x40, 0x91);
    ppu_tick(252);
    assert(ppu_w == 224 && ppu_xoff == 32);
    assert(ppu_rgb[0][10] == 0x000000u); /* world X < 0 */
    assert(ppu_rgb[0][20] == 0xFF0000u); /* reconstructed left margin */
    assert(ppu_rgb[0][32] == 0xFF0000u); /* native picture remains intact */
    assert(ppu_rgb[0][192] == 0xFF0000u); /* reconstructed right margin */
    wl3_test_active = 0;
    ppu_set_wl3_wide(0);
    return 0;
}
